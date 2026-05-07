#include "network/fluidnc_client.h"
#include "ui/ui_common.h"
#include "ui/tabs/control/ui_tab_control_probe.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <stdarg.h>

using namespace websockets;

// Static member initialization
WebsocketsClient FluidNCClient::webSocket;
FluidNCStatus FluidNCClient::currentStatus;
MachineConfig FluidNCClient::currentConfig;
uint32_t FluidNCClient::lastStatusRequestMs = 0;
bool FluidNCClient::initialized = false;
FluidNCMessageCallback FluidNCClient::messageCallback = nullptr;
FluidNCMessageCallback FluidNCClient::terminalCallback = nullptr;
bool FluidNCClient::autoReportingEnabled = false;
bool FluidNCClient::autoReportingAttempted = false;
uint32_t FluidNCClient::lastPollingMs = 0;
uint32_t FluidNCClient::lastGCodePollMs = 0;
uint32_t FluidNCClient::lastAutoReportAttemptMs = 0;
bool FluidNCClient::everConnectedSuccessfully = false;
// UART transport state
HardwareSerial* FluidNCClient::uartSerial = nullptr;
bool FluidNCClient::usingSerial = false;
static bool uartAutoReportSent = false;
static uint32_t uartAutoReportFallbackTime = 0;
// Serial read buffer
static char _serial_read_buf[1024];
static size_t _serial_read_pos = 0;  

void FluidNCClient::init() {
    if (initialized) return;
    
    Serial.println("[FluidNC] Initializing client");
    initialized = true;
}

bool FluidNCClient::connect(const MachineConfig &config) {
    if (!initialized) {
        Serial.println("[FluidNC] Error: Client not initialized");
        return false;
    }
    
    // Store config
    currentConfig = config;
    // If the machine is configured for UART, initialize UART transport and return
    if (config.connection_type == CONN_UART) {
        Serial.println("[FluidNC] Connecting to machine via UART");

        // Select UART instance
        if (config.uart_port == 0) uartSerial = &Serial0;
        else if (config.uart_port == 1) uartSerial = &Serial1;
        else uartSerial = &Serial2; // default to Serial2

        // Initialize UART: if pins provided, use begin with pins, otherwise use safe defaults
        int rx_pin = config.uart_rx_pin;
        int tx_pin = config.uart_tx_pin;
        
        // If using default pins (-1), check for hardware conflicts and use safe alternatives
        if (rx_pin < 0 || tx_pin < 0) {
            // For Basic hardware, GPIO16 conflicts with RGB display, so use GPIO18 (RX) and GPIO17 (TX)
            #ifndef HARDWARE_ADVANCE
            rx_pin = 18;  // Safe RX pin for Basic hardware
            tx_pin = 17;  // Safe TX pin for Basic hardware
            #else
            // For Advance hardware, use different safe pins if needed
            rx_pin = 18;  // Safe RX pin for Advance hardware
            tx_pin = 17;  // Safe TX pin for Advance hardware
            #endif
        }
        
        uartSerial->begin(config.uart_baud, SERIAL_8N1, rx_pin, tx_pin);
        delay(100);

        // CRITICAL: Flush any boot messages or stale data from UART RX buffer
        // FluidNC sends startup messages that can interfere with handshake
        unsigned long flushStart = millis();
        while (uartSerial->available() && millis() - flushStart < 200) {
            uartSerial->read();  // Consume and discard any buffered data
            delay(1);
        }
        delay(100);  // Give device time to settle

        // GRBL Handshake: Send status request and wait for response
        Serial.println("[FluidNC] UART: Sending handshake ('?')");
        uartSerial->println("?");
        uartSerial->flush();

        unsigned long start = millis();
        bool grblReady = false;
        String line = "";

        while (millis() - start < 1000) {
            while (uartSerial->available()) {
                char c = uartSerial->read();
                if (c == '\n' || c == '\r') {
                    if (line.length() > 0) {
                        Serial.printf("[FluidNC] UART: Received line: %s\n", line.c_str());
                        if (line.startsWith("<") && line.indexOf("|") > 0) {
                            // Valid status response format
                            grblReady = true;
                            Serial.println("[FluidNC] UART: ✓ Handshake successful!");
                            break;
                        }
                    }
                    line = "";
                } else if (c >= 32 && c <= 126) {
                    // Only accumulate printable characters
                    line += c;
                }
            }
            if (grblReady) break;
            delay(5);
        }

        if (grblReady) {
            Serial.println("[FluidNC] UART: Enabling auto-reporting (250ms interval)");
            uartSerial->println("$Report/Interval=250");
            uartSerial->flush();
            delay(50);

            currentStatus.is_connected = true;
            usingSerial = true;
            autoReportingEnabled = true;
            autoReportingAttempted = true;
            uartAutoReportSent = true;
            Serial.println("[FluidNC] UART: Connection established and ready!");
            return true;
        } else {
            Serial.println("[FluidNC] UART: ✗ Handshake failed - no status response received");
            usingSerial = false;
            uartSerial->end();
            return false;
        }
    }
    
    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FluidNC] Error: WiFi not connected");
        return false;
    }
    
    Serial.printf("[FluidNC] Connecting to %s:%d via WebSocket\n", 
                  config.fluidnc_url, config.websocket_port);
    
    // Resolve hostname if needed (mDNS support)
    String resolvedHost = String(config.fluidnc_url);
    IPAddress serverIP;
    if (resolvedHost.indexOf('.') == -1 || resolvedHost.endsWith(".local")) {
        // It's a hostname or mDNS name, try to resolve it
        Serial.printf("[FluidNC] Resolving hostname: %s\n", resolvedHost.c_str());
        
        // Try resolving with retries (mDNS can be slow to respond)
        bool resolved = false;
        for (int attempt = 0; attempt < 5 && !resolved; attempt++) {
            if (attempt > 0) {
                Serial.printf("[FluidNC] Retry attempt %d/5...\n", attempt + 1);
                delay(1000);  // Longer delay between retries for mDNS
            }
            
            if (resolvedHost.endsWith(".local")) {
                // Use MDNS.queryHost() for .local hostnames (strip the .local suffix)
                String hostname = resolvedHost.substring(0, resolvedHost.length() - 6);
                Serial.printf("[FluidNC] Using mDNS query for: %s\n", hostname.c_str());
                serverIP = MDNS.queryHost(hostname);
            } else {
                // Use standard DNS for non-.local hostnames
                WiFi.hostByName(resolvedHost.c_str(), serverIP);
            }
            
            // Validate that we got a real IP address (not 0.0.0.0)
            if (serverIP != IPAddress(0, 0, 0, 0)) {
                Serial.printf("[FluidNC] Resolved on attempt %d to IP: %s\n", attempt + 1, serverIP.toString().c_str());
                resolved = true;
            } else {
                Serial.printf("[FluidNC] Attempt %d returned invalid IP (0.0.0.0)\n", attempt + 1);
            }
        }
        
        if (!resolved) {
            Serial.printf("[FluidNC] Failed to resolve hostname: %s after 5 attempts\n", resolvedHost.c_str());
            Serial.println("[FluidNC] Tip: Try using the IP address instead, or check that mDNS is working on your network");
            return false;
        }
        resolvedHost = serverIP.toString();
        Serial.printf("[FluidNC] Using resolved IP: %s\n", resolvedHost.c_str());
    }
    
    // Set up event callbacks
    webSocket.onMessage(onMessageCallback);
    webSocket.onEvent(onEventsCallback);
    
    // Connect to WebSocket (ws://hostname:port/)
    char wsUrl[128];
    snprintf(wsUrl, sizeof(wsUrl), "ws://%s:%d/", resolvedHost.c_str(), config.websocket_port);
    bool connected = webSocket.connect(wsUrl);
    
    if (!connected) {
        Serial.println("[FluidNC] Initial connection failed");
        currentStatus.is_connected = false;
        return false;
    }
    
    Serial.println("[FluidNC] WebSocket connection initiated");
    currentStatus.is_connected = false;  // Will be set to true when first message received
    
    return true;
}

void FluidNCClient::disconnect() {
    if (usingSerial) {
        // For UART, just stop reading / end serial
        if (uartSerial) uartSerial->end();
        usingSerial = false;
        uartSerial = nullptr;
    } else {
        webSocket.close();
    }
    currentStatus.is_connected = false;
    currentStatus.state = STATE_DISCONNECTED;
}

void FluidNCClient::stopReconnectionAttempts() {
    Serial.println("[FluidNC] Stopping reconnection attempts");
    webSocket.close();
    currentStatus.is_connected = false;
    currentStatus.state = STATE_DISCONNECTED;
}

bool FluidNCClient::isConnected() {
    if (usingSerial) return currentStatus.is_connected;
    return currentStatus.is_connected && webSocket.available();
}

bool FluidNCClient::isAutoReporting() {
    return autoReportingEnabled;
}

void FluidNCClient::loop() {
    if (!initialized) return;
    // Block UART reading until a machine is selected
    if (currentConfig.name[0] == '\0') {
        if (uartSerial) { /// Flush UART to avoid buffer buildup
            while (uartSerial && uartSerial->available()) uartSerial->read();
        }
        return;
    }

    // Block UART reading until GRBL handshake is complete
    if (usingSerial && !currentStatus.is_connected) {
        if (uartSerial) { /// Ignore all incoming data until GRBL is ready
            while (uartSerial && uartSerial->available()) uartSerial->read();
        }
        return;
    }
    // Handle WebSocket events or UART input depending on transport
    if (usingSerial) {
        // --- UART Auto-Report Fallback (wenn FluidNC nichts sendet) ---
        if (!uartAutoReportSent && millis() > uartAutoReportFallbackTime) {
            uartAutoReportSent = true;
            Serial.println("[FluidNC] Fallback: enabling auto-report after delay...");
            uartSerial->println("$Report/Interval=250");
            uartSerial->flush();
            autoReportingAttempted = true;
        }
        // Read incoming bytes and split into lines
        // Limit bytes read per call to prevent blocking the main loop and UI
        int bytes_read = 0;
        const int MAX_BYTES_PER_LOOP = 32;  // Read max 32 bytes per loop call - return control very quickly
        
        while (uartSerial && uartSerial->available() && bytes_read < MAX_BYTES_PER_LOOP) {
            int c = uartSerial->read();
            bytes_read++;
            
            if (c == '\r') continue;
            if (c == '\n' || c == '>') {
                // If we hit '>' treat it as end of status token as well
                if (_serial_read_pos >= sizeof(_serial_read_buf) - 1) _serial_read_pos = sizeof(_serial_read_buf) - 1;
                _serial_read_buf[_serial_read_pos] = '\0';
                if (_serial_read_pos > 0) {
                    handleIncoming(_serial_read_buf);
                }
                _serial_read_pos = 0;
                // If c was '>' there might be a following newline; continue looping
                continue;
            }
            if (_serial_read_pos < sizeof(_serial_read_buf) - 1) {
                _serial_read_buf[_serial_read_pos++] = (char)c;
            }
            
            // Yield control periodically to allow other tasks and I2C transactions to complete
            if (bytes_read % 4 == 0) {
                yield();
            }
        }
        
        // Always call lv_timer_handler after UART reading to process touch input
        lv_timer_handler();

        // Allow auto-reporting / fallback polling logic when connected via UART
        if (!autoReportingAttempted && !autoReportingEnabled) {
            performFallbackPolling();
        }
        return;
    }

    // Handle WebSocket events - ArduinoWebsockets handles polling internally
    webSocket.poll();
    
    // Only check auto-reporting and polling if WebSocket is connected
    if (!webSocket.available()) {
        return;
    }
    
    // Check if auto-reporting timed out (no status received within 2 seconds of attempt)
    if (autoReportingAttempted && !autoReportingEnabled) {
        uint32_t now = millis();
        if (now - lastAutoReportAttemptMs >= 2000) {
            // No status received - assume auto-reporting failed
            Serial.println("[FluidNC] Auto-reporting timeout - switching to fallback polling");
            autoReportingAttempted = false;  // Allow fallback polling to start
        }
    }
    
    // Only perform fallback polling if:
    // 1. Auto-reporting attempt completed (either succeeded or timed out)
    // 2. Auto-reporting is not enabled
    if (!autoReportingAttempted && !autoReportingEnabled) {
        performFallbackPolling();
    }
}

const FluidNCStatus& FluidNCClient::getStatus() {
    return currentStatus;
}

void FluidNCClient::sendCommand(const char* command) {
    if (!currentStatus.is_connected) {
        Serial.println("[FluidNC] Error: Not connected");
        return;
    }

    Serial.printf("[FluidNC] Sending command: %s\n", command);
    if (usingSerial && uartSerial) {
        // Ensure newline termination
        String s = String(command);
        if (!s.endsWith("\n")) s += "\n";
        uartSerial->print(s);
    } else {
        webSocket.send(command);
    }
}

void FluidNCClient::requestStatusReport() {
    if (!currentStatus.is_connected) return;
    // Send status query command (realtime command)
    sendCommand("?");
}

String FluidNCClient::getMachineIP() {
    if (!currentStatus.is_connected) return "";
    
    // Get URL from config
    String url = String(currentConfig.fluidnc_url);
    
    // Extract IP from URL (may already be just an IP address)
    // If it starts with a protocol, remove it
    int protocolEnd = url.indexOf("://");
    if (protocolEnd >= 0) {
        url = url.substring(protocolEnd + 3);
    }
    
    // Remove port if present
    int portStart = url.indexOf(":");
    if (portStart > 0) {
        url = url.substring(0, portStart);
    }
    
    // Remove path if present
    int pathStart = url.indexOf("/");
    if (pathStart > 0) {
        url = url.substring(0, pathStart);
    }
    
    return url;
}

void FluidNCClient::setMessageCallback(FluidNCMessageCallback callback) {
    messageCallback = callback;
    Serial.println("[FluidNC] Message callback registered");
}

void FluidNCClient::clearMessageCallback() {
    messageCallback = nullptr;
    Serial.println("[FluidNC] Message callback cleared");
}

void FluidNCClient::setTerminalCallback(FluidNCMessageCallback callback) {
    terminalCallback = callback;
    Serial.println("[FluidNC] Terminal callback registered");
}

void FluidNCClient::clearTerminalCallback() {
    terminalCallback = nullptr;
    Serial.println("[FluidNC] Terminal callback cleared");
}

void FluidNCClient::onMessageCallback(WebsocketsMessage message) {
    // Route to shared handler
    handleIncoming(message.c_str());
}

void FluidNCClient::handleIncoming(const char* payload) {
    // Shared handler for both WebSocket and UART incoming messages
    if (!payload || payload[0] == '\0') return;

    // Only log non-status messages to reduce serial spam
    if (payload[0] != '<') {
        Serial.printf("[FluidNC] Received: %s\n", payload);
    }

    // Call message callback if registered (for file lists, etc.)
    if (messageCallback) {
        messageCallback(payload);
    }

    // Call terminal callback if registered (for terminal display)
    // Terminal tab will filter out status messages (starting with '<')
    if (terminalCallback) {
        terminalCallback(payload);
    }

    // Parse different message types
    if (payload[0] == '<') {
        // Status report: <Idle|MPos:0.000,0.000,0.000|WPos:0.000,0.000,0.000|...>
        parseStatusReport(payload);
        // Auto-Reporting aktivieren, sobald erster Status empfangen wird
        if (!currentStatus.is_connected && usingSerial) {
            currentStatus.is_connected = true;

            if (!autoReportingAttempted) {
                Serial.println("[FluidNC] First status received -> enabling auto-reporting");
                attemptEnableAutoReporting();
            }
        }
    } else if (strncmp(payload, "[GC:", 4) == 0) {
        // GCode parser state: [GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]
        parseGCodeState(payload);
    } else if (payload[0] == '[') {
        // Other realtime feedback: [MSG:...], [G92:...], etc.
        parseRealtimeFeedback(payload);
    }
}

void FluidNCClient::onEventsCallback(WebsocketsEvent event, String data) {
    switch(event) {
        case WebsocketsEvent::ConnectionOpened:
            Serial.println("[FluidNC] WebSocket connected");
            // Don't set is_connected yet - wait for first status report
            currentStatus.state = STATE_IDLE;
            currentStatus.last_update_ms = millis();
            
            // Initialize polling timestamps to allow immediate fallback polling if auto-reporting fails
            lastPollingMs = millis() - 1000;
            lastGCodePollMs = millis() - 10000;
            
            // Attempt to enable automatic status reporting
            attemptEnableAutoReporting();
            break;
            
        case WebsocketsEvent::ConnectionClosed:
            Serial.println("[FluidNC] WebSocket disconnected");
            
            // Only show popup if we've ever successfully received a status report
            if (everConnectedSuccessfully) {
                // Build error message
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), 
                        "Lost connection to machine.\n\nCheck network connection and\nmachine power, then restart.");
                
                // Show error dialog
                UICommon::showConnectionErrorDialog("Machine Disconnected", error_msg);
            }
            
            currentStatus.is_connected = false;
            currentStatus.state = STATE_DISCONNECTED;
            break;
            
        case WebsocketsEvent::GotPing:
            Serial.println("[FluidNC] Received ping");
            break;
            
        case WebsocketsEvent::GotPong:
            Serial.println("[FluidNC] Received pong");
            break;
    }
}

void FluidNCClient::parseStatusReport(const char* message) {
    // Example: <Idle|MPos:0.000,0.000,0.000|FS:0,0|Ov:100,100,100>
    // Or with WCO: <Idle|MPos:0.000,0.000,0.000|FS:0,0|WCO:0.000,0.000,0.000>
    
    // If we receive a status report while auto-reporting was attempted, mark it as enabled
    if (autoReportingAttempted && !autoReportingEnabled) {
        autoReportingEnabled = true;
        autoReportingAttempted = false;  // Clear the attempt flag
        Serial.println("[FluidNC] ✓ Auto-reporting confirmed (status received)");
    }
    
    // If we receive ANY status report and not connected yet, mark as connected
    // This handles both auto-reporting and fallback polling
    if (!currentStatus.is_connected) {
        currentStatus.is_connected = true;
        Serial.println("[FluidNC] ✓ Connection established (status received)");
        
        // Hide connecting popup
        UICommon::hideConnectingPopup();
        
        // Also hide error dialog if showing
        UICommon::hideConnectionErrorDialog();
    }
    
    // Only log status every 5 seconds to reduce spam
    static uint32_t lastStatusLog = 0;
    uint32_t now = millis();
    if (now - lastStatusLog >= 5000) {
        Serial.printf("[FluidNC] Status update (5s): %s\n", message);
        lastStatusLog = now;
    }
    
    currentStatus.last_update_ms = millis();
    
    // Mark that we've successfully received at least one status report
    // This flag is used to distinguish between initial connection handshake failures
    // and actual disconnections after successful communication
    if (!everConnectedSuccessfully) {
        everConnectedSuccessfully = true;
        Serial.println("[FluidNC] ✓ First status report received - connection validated");
    }
    
    // Track previous state for state change detection
    static MachineState previousState = STATE_DISCONNECTED;
    MachineState newState = currentStatus.state;
    
    // Parse machine state
    if (strstr(message, "<Idle")) {
        newState = STATE_IDLE;
    } else if (strstr(message, "<Run")) {
        newState = STATE_RUN;
    } else if (strstr(message, "<Hold")) {
        newState = STATE_HOLD;
    } else if (strstr(message, "<Jog")) {
        newState = STATE_JOG;
    } else if (strstr(message, "<Alarm")) {
        newState = STATE_ALARM;
    } else if (strstr(message, "<Door")) {
        newState = STATE_DOOR;
    } else if (strstr(message, "<Check")) {
        newState = STATE_CHECK;
    } else if (strstr(message, "<Home")) {
        newState = STATE_HOME;
    } else if (strstr(message, "<Sleep")) {
        newState = STATE_SLEEP;
    }
    
    // Detect state change to IDLE from HOLD or RUN - retry auto-reporting
    if (newState == STATE_IDLE && (previousState == STATE_HOLD || previousState == STATE_RUN)) {
        if (!autoReportingEnabled) {
            Serial.println("[FluidNC] Machine returned to IDLE - retrying auto-reporting");
            attemptEnableAutoReporting();
        }
    }
    
    // Update state and track for next comparison
    currentStatus.state = newState;
    previousState = newState;
    
    // Parse machine position (MPos:x,y,z)
    const char* mpos = strstr(message, "MPos:");
    if (mpos) {
        sscanf(mpos + 5, "%f,%f,%f", &currentStatus.mpos_x, &currentStatus.mpos_y, &currentStatus.mpos_z);
    }
    
    // Parse work coordinate offset (WCO:x,y,z) - sent periodically by FluidNC
    const char* wco = strstr(message, "WCO:");
    if (wco) {
        sscanf(wco + 4, "%f,%f,%f", &currentStatus.wco_x, &currentStatus.wco_y, &currentStatus.wco_z);
        Serial.printf("[FluidNC] WCO updated: (%.3f,%.3f,%.3f)\n", 
                      currentStatus.wco_x, currentStatus.wco_y, currentStatus.wco_z);
    }
    
    // Calculate work position: WPos = MPos - WCO
    // FluidNC typically only sends MPos in every status report, but includes WCO periodically
    currentStatus.wpos_x = currentStatus.mpos_x - currentStatus.wco_x;
    currentStatus.wpos_y = currentStatus.mpos_y - currentStatus.wco_y;
    currentStatus.wpos_z = currentStatus.mpos_z - currentStatus.wco_z;
    
    // Parse work position directly (WPos:x,y,z) - rarely sent, but handle it
    const char* wpos = strstr(message, "WPos:");
    if (wpos) {
        sscanf(wpos + 5, "%f,%f,%f", &currentStatus.wpos_x, &currentStatus.wpos_y, &currentStatus.wpos_z);
    }
    
    // Parse feed and spindle (FS:feed,spindle)
    const char* fs = strstr(message, "FS:");
    if (fs) {
        sscanf(fs + 3, "%f,%f", &currentStatus.feed_rate, &currentStatus.spindle_speed);
        Serial.printf("[FluidNC] Parsed FS: feed=%.0f, spindle=%.0f\n", 
                      currentStatus.feed_rate, currentStatus.spindle_speed);
    }
    
    // Parse overrides (Ov:feed,rapid,spindle)
    const char* ov = strstr(message, "Ov:");
    if (ov) {
        sscanf(ov + 3, "%f,%f,%f", &currentStatus.feed_override, &currentStatus.rapid_override, &currentStatus.spindle_override);
        Serial.printf("[FluidNC] Parsed Ov: feed=%.0f%%, rapid=%.0f%%, spindle=%.0f%%\n", 
                      currentStatus.feed_override, currentStatus.rapid_override, currentStatus.spindle_override);
    }
    
    // Parse SD card file progress (SD:percent,filename)
    const char* sd = strstr(message, "SD:");
    if (sd) {
        // Extract percent and filename
        float percent = 0;
        char filename_buf[128] = {0};
        
        // Parse: SD:12.5,filename.gcode or SD:100.0,file.nc
        const char* comma = strchr(sd + 3, ',');
        if (comma) {
            sscanf(sd + 3, "%f", &percent);
            strncpy(filename_buf, comma + 1, sizeof(filename_buf) - 1);
            
            // Remove any trailing > or whitespace
            char* end = strchr(filename_buf, '>');
            if (end) *end = '\0';
            end = strchr(filename_buf, '|');
            if (end) *end = '\0';
            
            // Update status
            currentStatus.is_sd_printing = true;
            currentStatus.sd_percent = percent;
            strncpy(currentStatus.sd_filename, filename_buf, sizeof(currentStatus.sd_filename) - 1);
            currentStatus.sd_filename[sizeof(currentStatus.sd_filename) - 1] = '\0';
            
            // Track start time and calculate elapsed time
            if (currentStatus.sd_start_time_ms == 0) {
                currentStatus.sd_start_time_ms = millis();
            }
            currentStatus.sd_elapsed_ms = millis() - currentStatus.sd_start_time_ms;
            
            Serial.printf("[FluidNC] SD Progress: %.1f%% - %s (Elapsed: %lums)\n",
                          percent, currentStatus.sd_filename, currentStatus.sd_elapsed_ms);
        }
    } else {
        // No SD: field means not printing from SD
        if (currentStatus.is_sd_printing) {
            Serial.println("[FluidNC] SD file completed or stopped");
        }
        currentStatus.is_sd_printing = false;
        currentStatus.sd_percent = 0;
        currentStatus.sd_start_time_ms = 0;
        currentStatus.sd_elapsed_ms = 0;
        currentStatus.sd_filename[0] = '\0';
    }
    
    // Parse modal states (Pn:, WCO:, etc.)
    // Note: Full parser state might come in separate $G response
    
    Serial.printf("[FluidNC] Status: State=%d, MPos=(%.3f,%.3f,%.3f), WPos=(%.3f,%.3f,%.3f)\n",
                  currentStatus.state,
                  currentStatus.mpos_x, currentStatus.mpos_y, currentStatus.mpos_z,
                  currentStatus.wpos_x, currentStatus.wpos_y, currentStatus.wpos_z);
}

void FluidNCClient::parseRealtimeFeedback(const char* message) {
    // Handle realtime feedback messages like [MSG:...], [G92:...], [PRB:...], etc.
    if (usingSerial && !uartAutoReportSent) {
        uartAutoReportSent = true;
        Serial.println("[FluidNC] UART ready, enabling auto-report...");
        uartSerial->println("$Report/Interval=250");
        uartSerial->flush();
        autoReportingAttempted = true;
    }
    
    Serial.printf("[FluidNC] Feedback: %s\n", message);
    
    // Check for probe result message: [PRB:x,y,z:success]
    // Example: [PRB:151.000,149.000,-137.505:1] (success=1) or [PRB:0.000,0.000,0.000:0] (failure=0)
    if (strncmp(message, "[PRB:", 5) == 0) {
        float x, y, z;
        int success;
        if (sscanf(message + 5, "%f,%f,%f:%d", &x, &y, &z, &success) == 4) {
            // Update probe tab result display with coordinates
            UITabControlProbe::updateResult(x, y, z, success != 0);
            
            Serial.printf("[FluidNC] Probe %s at (%.3f, %.3f, %.3f)\n", 
                         success ? "SUCCESS" : "FAILED", x, y, z);
        }
    }
    
    // Check for auto-report confirmation message
    if (strstr(message, "websocket auto report interval set") != nullptr) {
        Serial.println("[FluidNC] ✓ Auto-report confirmed - automatic reporting enabled");
        autoReportingEnabled = true;
        
        if (!currentStatus.is_connected) {
            currentStatus.is_connected = true;
            Serial.println("[FluidNC] ✓ Connection established");
            
            // Hide connecting popup
            UICommon::hideConnectingPopup();
            
            // Also hide error dialog if showing (connection succeeded after error)
            UICommon::hideConnectionErrorDialog();
        }
    }
    
    // Extract message content from [MSG:...] format
    if (strncmp(message, "[MSG:", 5) == 0) {
        // Find the closing bracket
        const char* end = strchr(message, ']');
        if (end) {
            // Copy message content (skip "[MSG:" and "]")
            size_t len = end - (message + 5);
            if (len >= sizeof(currentStatus.last_message)) {
                len = sizeof(currentStatus.last_message) - 1;
            }
            strncpy(currentStatus.last_message, message + 5, len);
            currentStatus.last_message[len] = '\0';
        }
    } else {
        // For other feedback messages, store the whole thing
        strncpy(currentStatus.last_message, message, sizeof(currentStatus.last_message) - 1);
        currentStatus.last_message[sizeof(currentStatus.last_message) - 1] = '\0';
    }
}

void FluidNCClient::parseGCodeState(const char* message) {
    // Example: [GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]
    // Parse modal states from GCode parser state report
    
    Serial.printf("[FluidNC] GCode State: %s\n", message);
    
    // Extract modal values by searching for specific patterns
    const char* ptr = message + 4;  // Skip "[GC:"
    
    // Parse motion mode (G0, G1, G2, G3, G38.2, G38.3, G38.4, G38.5, G80)
    if (const char* g0 = strstr(ptr, "G0 ")) strcpy(currentStatus.modal_motion, "G0");
    else if (const char* g1 = strstr(ptr, "G1 ")) strcpy(currentStatus.modal_motion, "G1");
    else if (const char* g2 = strstr(ptr, "G2 ")) strcpy(currentStatus.modal_motion, "G2");
    else if (const char* g3 = strstr(ptr, "G3 ")) strcpy(currentStatus.modal_motion, "G3");
    else if (const char* g80 = strstr(ptr, "G80")) strcpy(currentStatus.modal_motion, "G80");
    
    // Parse work coordinate system (G54-G59)
    if (strstr(ptr, "G54")) strcpy(currentStatus.modal_wcs, "G54");
    else if (strstr(ptr, "G55")) strcpy(currentStatus.modal_wcs, "G55");
    else if (strstr(ptr, "G56")) strcpy(currentStatus.modal_wcs, "G56");
    else if (strstr(ptr, "G57")) strcpy(currentStatus.modal_wcs, "G57");
    else if (strstr(ptr, "G58")) strcpy(currentStatus.modal_wcs, "G58");
    else if (strstr(ptr, "G59")) strcpy(currentStatus.modal_wcs, "G59");
    
    // Parse plane (G17, G18, G19)
    if (strstr(ptr, "G17")) strcpy(currentStatus.modal_plane, "G17");
    else if (strstr(ptr, "G18")) strcpy(currentStatus.modal_plane, "G18");
    else if (strstr(ptr, "G19")) strcpy(currentStatus.modal_plane, "G19");
    
    // Parse units (G20=inches, G21=mm)
    if (strstr(ptr, "G20")) strcpy(currentStatus.modal_units, "G20");
    else if (strstr(ptr, "G21")) strcpy(currentStatus.modal_units, "G21");
    
    // Parse distance mode (G90=absolute, G91=incremental)
    if (strstr(ptr, "G90")) strcpy(currentStatus.modal_distance, "G90");
    else if (strstr(ptr, "G91")) strcpy(currentStatus.modal_distance, "G91");
    
    // Parse spindle state (M3=CW, M4=CCW, M5=off)
    if (strstr(ptr, "M3 ")) strcpy(currentStatus.modal_spindle, "M3");
    else if (strstr(ptr, "M4 ")) strcpy(currentStatus.modal_spindle, "M4");
    else if (strstr(ptr, "M5")) strcpy(currentStatus.modal_spindle, "M5");
    
    // Parse coolant state (M7=mist, M8=flood, M9=off)
    if (strstr(ptr, "M7 ")) strcpy(currentStatus.modal_coolant, "M7");
    else if (strstr(ptr, "M8 ")) strcpy(currentStatus.modal_coolant, "M8");
    else if (strstr(ptr, "M9")) strcpy(currentStatus.modal_coolant, "M9");
    
    // Parse tool number (T0, T1, etc.)
    const char* tool = strstr(ptr, " T");
    if (tool) {
        int toolNum;
        if (sscanf(tool, " T%d", &toolNum) == 1) {
            snprintf(currentStatus.modal_tool, sizeof(currentStatus.modal_tool), "T%d", toolNum);
        }
    }
    
    // Parse feed rate (F) - programmed feed rate in mm/min
    const char* feed = strstr(ptr, " F");
    if (feed) {
        float feedValue;
        if (sscanf(feed, " F%f", &feedValue) == 1) {
            // Only update if not already set by status report
            if (currentStatus.feed_rate == 0.0f) {
                currentStatus.feed_rate = feedValue;
            }
        }
    }
    
    // Parse spindle speed (S) - programmed spindle speed in RPM
    const char* spindle = strstr(ptr, " S");
    if (spindle) {
        float spindleValue;
        if (sscanf(spindle, " S%f", &spindleValue) == 1) {
            // Only update if not already set by status report
            if (currentStatus.spindle_speed == 0.0f) {
                currentStatus.spindle_speed = spindleValue;
            }
        }
    }
    
    Serial.printf("[FluidNC] Parsed modals: Motion=%s, WCS=%s, Plane=%s, Units=%s, Distance=%s, Spindle=%s, Coolant=%s, Tool=%s, Feed=%.0f, SpindleSpeed=%.0f\n",
                  currentStatus.modal_motion, currentStatus.modal_wcs, currentStatus.modal_plane,
                  currentStatus.modal_units, currentStatus.modal_distance, currentStatus.modal_spindle,
                  currentStatus.modal_coolant, currentStatus.modal_tool,
                  currentStatus.feed_rate, currentStatus.spindle_speed);
}

float FluidNCClient::extractFloat(const char* str, const char* key) {
    const char* pos = strstr(str, key);
    if (!pos) return 0.0f;
    
    return atof(pos + strlen(key));
}

void FluidNCClient::extractString(const char* str, const char* key, char* dest, size_t maxLen) {
    const char* pos = strstr(str, key);
    if (!pos) {
        dest[0] = '\0';
        return;
    }
    
    pos += strlen(key);
    const char* end = strchr(pos, '|');
    if (!end) end = strchr(pos, '>');
    if (!end) end = pos + strlen(pos);
    
    size_t len = end - pos;
    if (len >= maxLen) len = maxLen - 1;
    
    strncpy(dest, pos, len);
    dest[len] = '\0';
}

void FluidNCClient::attemptEnableAutoReporting() {
    Serial.println("[FluidNC] Attempting to enable automatic reporting (250ms)");
    sendCommand("$Report/Interval=250\n");
    
    autoReportingAttempted = true;
    autoReportingEnabled = false;  // Will be set true when we receive status
    lastAutoReportAttemptMs = millis();
}

void FluidNCClient::performFallbackPolling() {
    // If auto-reporting is enabled, no need to poll
    if (autoReportingEnabled) {
        return;
    }
    
    uint32_t now = millis();
    
    // Send status poll ("?") every 1 second
    if (now - lastPollingMs >= 1000) {
        Serial.println("[FluidNC] Fallback polling: sending '?'");
        sendCommand("?");
        lastPollingMs = now;
    }
    
    // Send GCode parser state poll ("$G") every 10 seconds
    if (now - lastGCodePollMs >= 10000) {
        Serial.println("[FluidNC] Fallback polling: sending '$G'");
        sendCommand("$G\n");
        lastGCodePollMs = now;
    }
}

