/*
    VFDSpindle.cpp

    This is for a VFD based spindles via RS485 Modbus. The details of the 
    VFD protocol heavily depend on the VFD in question here. We have some 
    implementations, but if yours is not here, the place to start is the 
    manual. This VFD class implements the modbus functionality.

    Part of Grbl_ESP32
    2020 -  Bart Dring
    2020 -  Stefan de Bruijn

    Grbl is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    Grbl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

                         WARNING!!!!
    VFDs are very dangerous. They have high voltages and are very powerful
    Remove power before changing bits.

    TODO:
      - We can report spindle_state and rpm better with VFD's that support 
        either mode, register RPM or actual RPM.
      - Destructor should break down the task.
      - Move min/max RPM to protected members.

*/
#include "VFDSpindle.h"

const uart_port_t VFD_RS485_UART_PORT  = UART_NUM_2;  // hard coded for this port right now
const int         VFD_RS485_BUF_SIZE   = 127;
const int         VFD_RS485_QUEUE_SIZE = 10;   // numv\ber of commands that can be queued up.
const int         RESPONSE_WAIT_TICKS  = 50;   // how long to wait for a response
const int         VFD_RS485_POLL_RATE  = 200;  // in milliseconds between commands

// OK to change these
// #define them in your machine definition file if you want different values
#ifndef VFD_RS485_ADDR
#    define VFD_RS485_ADDR 0x01
#endif

namespace Spindles {
    QueueHandle_t VFD::vfd_cmd_queue     = nullptr;
    TaskHandle_t  VFD::vfd_cmdTaskHandle = nullptr;

    // The communications task
    void VFD::vfd_cmd_task(void* pvParameters) {
        static bool unresponsive = false;  // to pop off a message once each time it becomes unresponsive
        static int  pollidx      = 0;

        VFD*          instance = static_cast<VFD*>(pvParameters);
        ModbusCommand next_cmd;
        uint8_t       rx_message[VFD_RS485_MAX_MSG_SIZE];

        while (true) {
            response_parser parser = nullptr;

            next_cmd.msg[0] = VFD_RS485_ADDR;  // Always default to this

            // First check if we should ask the VFD for the max RPM value as part of the initialization. We
            // should also query this is max_rpm is 0, because that means a previous initialization failed:
            if (pollidx == 0 || (instance->_max_rpm == 0 && (parser = instance->get_max_rpm(next_cmd)) != nullptr)) {
                pollidx           = 1;
                next_cmd.critical = true;
            } else {
                next_cmd.critical = false;
            }

            // If we don't have a parser, the queue goes first. During idle, we can grab a parser.
            if (parser == nullptr && xQueueReceive(vfd_cmd_queue, &next_cmd, 0) != pdTRUE) {
                // We poll in a cycle. Note that the switch will fall through unless we encounter a hit.
                // The weakest form here is 'get_status_ok' which should be implemented if the rest fails.
                switch (pollidx) {
                    case 1:
                        parser = instance->get_current_rpm(next_cmd);
                        if (parser) {
                            pollidx = 2;
                            break;
                        }
                        // fall through intentionally:
                    case 2:
                        parser = instance->get_current_direction(next_cmd);
                        if (parser) {
                            pollidx = 3;
                            break;
                        }
                        // fall through intentionally:
                    case 3:
                        parser  = instance->get_status_ok(next_cmd);
                        pollidx = 1;

                        // we could complete this in case parser == nullptr with some ifs, but let's
                        // just keep it easy and wait an iteration.
                        break;
                }

                // If we have no parser, that means get_status_ok is not implemented (and we have
                // nothing resting in our queue). Let's fall back on a simple continue.
                if (parser == nullptr) {
                    vTaskDelay(VFD_RS485_POLL_RATE);
                    continue;  // main while loop
                }
            }

            {
                // Grabbed the command. Add the CRC16 checksum:
                auto crc16 = ModRTU_CRC(next_cmd.msg, next_cmd.tx_length);

                next_cmd.tx_length += 2;
                next_cmd.rx_length += 2;

                // add the calculated Crc to the message
                next_cmd.msg[next_cmd.tx_length - 1] = (crc16 & 0xFF00) >> 8;
                next_cmd.msg[next_cmd.tx_length - 2] = (crc16 & 0xFF);

#ifdef VFD_DEBUG_MODE
                if (parser == nullptr) {
                    report_hex_msg(next_cmd.msg, "RS485 Tx: ", next_cmd.tx_length);
                }
#endif
            }

            // Assume for the worst, and retry...
            int retry_count = 0;
            for (; retry_count < MAX_RETRIES; ++retry_count) {
                // Flush the UART and write the data:
                uart_flush(VFD_RS485_UART_PORT);
                uart_write_bytes(VFD_RS485_UART_PORT, reinterpret_cast<const char*>(next_cmd.msg), next_cmd.tx_length);

                // Read the response
                uint16_t read_length = uart_read_bytes(VFD_RS485_UART_PORT, rx_message, next_cmd.rx_length, RESPONSE_WAIT_TICKS);

                // Generate crc16 for the response:
                auto crc16response = ModRTU_CRC(rx_message, next_cmd.rx_length - 2);

                if (read_length == next_cmd.rx_length &&                             // check expected length
                    rx_message[0] == VFD_RS485_ADDR &&                               // check address
                    rx_message[read_length - 1] == (crc16response & 0xFF00) >> 8 &&  // check CRC byte 1
                    rx_message[read_length - 2] == (crc16response & 0xFF)) {         // check CRC byte 1

                    // success
                    unresponsive = false;
                    retry_count  = MAX_RETRIES + 1;  // stop retry'ing

                    // Should we parse this?
                    if (parser != nullptr && !parser(rx_message, instance)) {
#ifdef VFD_DEBUG_MODE
                        report_hex_msg(next_cmd.msg, "RS485 Tx: ", next_cmd.tx_length);
                        report_hex_msg(rx_message, "RS485 Rx: ", read_length);
#endif

                        // Not succesful! Now what?
                        unresponsive = true;
                        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Spindle RS485 did not give a satisfying response");
                    }
                } else {
#ifdef VFD_DEBUG_MODE
                    report_hex_msg(next_cmd.msg, "RS485 Tx: ", next_cmd.tx_length);
                    report_hex_msg(rx_message, "RS485 Rx: ", read_length);

                    if (read_length != 0) {
                        if (rx_message[0] != VFD_RS485_ADDR) {
                            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 received message from other modbus device");
                        } else if (read_length != next_cmd.rx_length) {
                            grbl_msg_sendf(CLIENT_SERIAL,
                                           MsgLevel::Info,
                                           "RS485 received message of unexpected length; expected %d, got %d",
                                           int(next_cmd.rx_length),
                                           int(read_length));
                        } else {
                            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 CRC check failed");
                        }
                    } else {
                        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 No response");
                    }
#endif

                    // Wait a bit before we retry. Set the delay to poll-rate. Not sure
                    // if we should use a different value...
                    vTaskDelay(VFD_RS485_POLL_RATE);
                }
            }

            if (retry_count == MAX_RETRIES) {
                if (!unresponsive) {
                    grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Spindle RS485 Unresponsive %d", next_cmd.rx_length);
                    if (next_cmd.critical) {
                        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Critical Spindle RS485 Unresponsive");
                        system_set_exec_alarm(ExecAlarm::SpindleControl);
                    }
                    unresponsive = true;
                }
            }

            vTaskDelay(VFD_RS485_POLL_RATE);  // TODO: What is the best value here?
        }
    }

    // ================== Class methods ==================================
    void VFD::default_modbus_settings(uart_config_t& uart) {
        // Default is 9600 8N1, which is sane for most VFD's:
        uart.baud_rate = 9600;
        uart.data_bits = UART_DATA_8_BITS;
        uart.parity    = UART_PARITY_DISABLE;
        uart.stop_bits = UART_STOP_BITS_1;
    }

    void VFD::init() {
        vfd_ok = false;  // initialize

        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Initializing RS485 VFD spindle");

        // fail if required items are not defined
        if (!get_pins_and_settings()) {
            vfd_ok = false;
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 VFD spindle errors");
            return;
        }

        // this allows us to init() again later.
        // If you change certain settings, init() gets called agian
        uart_driver_delete(VFD_RS485_UART_PORT);

        uart_config_t uart_config;
        default_modbus_settings(uart_config);

        // Overwrite with user defined defines:
#ifdef VFD_RS485_BAUD_RATE
        uart_config.baud_rate = VFD_RS485_BAUD_RATE;
#endif
#ifdef VFD_RS485_PARITY
        uart_config.parity = VFD_RS485_PARITY;
#endif

        uart_config.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;

        if (uart_param_config(VFD_RS485_UART_PORT, &uart_config) != ESP_OK) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 VFD uart parameters failed");
            return;
        }

        if (uart_set_pin(VFD_RS485_UART_PORT, _txd_pin, _rxd_pin, _rts_pin, UART_PIN_NO_CHANGE) != ESP_OK) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 VFD uart pin config failed");
            return;
        }

        if (uart_driver_install(VFD_RS485_UART_PORT, VFD_RS485_BUF_SIZE * 2, 0, 0, NULL, 0) != ESP_OK) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 VFD uart driver install failed");
            return;
        }

        if (uart_set_mode(VFD_RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX) != ESP_OK) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "RS485 VFD uart set half duplex failed");
            return;
        }

        // Initialization is complete, so now it's okay to run the queue task:
        if (!_task_running) {  // init can happen many times, we only want to start one task
            vfd_cmd_queue = xQueueCreate(VFD_RS485_QUEUE_SIZE, sizeof(ModbusCommand));
            xTaskCreatePinnedToCore(vfd_cmd_task,         // task
                                    "vfd_cmdTaskHandle",  // name for task
                                    2048,                 // size of task stack
                                    this,                 // parameters
                                    1,                    // priority
                                    &vfd_cmdTaskHandle,
                                    0  // core
            );
            _task_running = true;
        }

        is_reversable = true;  // these VFDs are always reversable
        use_delays    = true;
        vfd_ok        = true;

        // Initially we initialize this to 0; over time, we might poll better information from the VFD.
        _current_rpm   = 0;
        _current_state = SpindleState::Disable;

        config_message();
    }

    // Checks for all the required pin definitions
    // It returns a message for each missing pin
    // Returns true if all pins are defined.
    bool VFD::get_pins_and_settings() {
        bool pins_settings_ok = true;

#ifdef VFD_RS485_TXD_PIN
        _txd_pin = VFD_RS485_TXD_PIN;
#else
        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Undefined VFD_RS485_TXD_PIN");
        pins_settings_ok = false;
#endif

#ifdef VFD_RS485_RXD_PIN
        _rxd_pin = VFD_RS485_RXD_PIN;
#else
        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Undefined VFD_RS485_RXD_PIN");
        pins_settings_ok = false;
#endif

#ifdef VFD_RS485_RTS_PIN
        _rts_pin = VFD_RS485_RTS_PIN;
#else
        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Undefined VFD_RS485_RTS_PIN");
        pins_settings_ok = false;
#endif

        if (laser_mode->get()) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "VFD spindle disabled in laser mode. Set $GCode/LaserMode=Off and restart");
            pins_settings_ok = false;
        }

        _min_rpm = rpm_min->get();
        _max_rpm = rpm_max->get();

        return pins_settings_ok;
    }

    void VFD::config_message() {
        grbl_msg_sendf(CLIENT_SERIAL,
                       MsgLevel::Info,
                       "VFD RS485  Tx:%s Rx:%s RTS:%s",
                       pinName(_txd_pin).c_str(),
                       pinName(_rxd_pin).c_str(),
                       pinName(_rts_pin).c_str());
    }

    void VFD::set_state(SpindleState state, uint32_t rpm) {
        if (sys.abort) {
            return;  // Block during abort.
        }

        bool critical = (sys.state == State::Cycle || state != SpindleState::Disable);

        if (_current_state != state) {  // already at the desired state. This function gets called a lot.
            set_mode(state, critical);  // critical if we are in a job
            set_rpm(rpm);
            if (state == SpindleState::Disable) {
                sys.spindle_speed = 0;
                if (_current_state != state) {
                    mc_dwell(spindle_delay_spindown->get());
                }
            } else {
                if (_current_state != state) {
                    mc_dwell(spindle_delay_spinup->get());
                }
            }
        } else {
            if (_current_rpm != rpm) {
                set_rpm(rpm);
            }
        }

        _current_state = state;  // store locally for faster get_state()

        sys.report_ovr_counter = 0;  // Set to report change immediately

        return;
    }

    bool VFD::set_mode(SpindleState mode, bool critical) {
        if (!vfd_ok) {
            return false;
        }

        ModbusCommand mode_cmd;
        mode_cmd.msg[0] = VFD_RS485_ADDR;

        direction_command(mode, mode_cmd);

        if (mode == SpindleState::Disable) {
            if (!xQueueReset(vfd_cmd_queue)) {
                grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "VFD spindle off, queue could not be reset");
            }
        }

        mode_cmd.critical = critical;
        _current_state    = mode;

        if (xQueueSend(vfd_cmd_queue, &mode_cmd, 0) != pdTRUE) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "VFD Queue Full");
        }

        return true;
    }

    uint32_t VFD::set_rpm(uint32_t rpm) {
        if (!vfd_ok) {
            return 0;
        }

#ifdef VFD_DEBUG_MODE
        grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "Setting spindle speed to %d rpm (%d, %d)", int(rpm), int(_min_rpm), int(_max_rpm));
#endif

        // apply override
        rpm = rpm * sys.spindle_speed_ovr / 100;  // Scale by spindle speed override value (uint8_t percent)

        // apply limits
        if ((_min_rpm >= _max_rpm) || (rpm >= _max_rpm)) {
            rpm = _max_rpm;
        } else if (rpm != 0 && rpm <= _min_rpm) {
            rpm = _min_rpm;
        }

        sys.spindle_speed = rpm;

        if (rpm == _current_rpm) {  // prevent setting same RPM twice
            return rpm;
        }

        _current_rpm = rpm;

        // TODO add the speed modifiers override, linearization, etc.

        ModbusCommand rpm_cmd;
        rpm_cmd.msg[0] = VFD_RS485_ADDR;

        set_speed_command(rpm, rpm_cmd);

        rpm_cmd.critical = false;

        if (xQueueSend(vfd_cmd_queue, &rpm_cmd, 0) != pdTRUE) {
            grbl_msg_sendf(CLIENT_SERIAL, MsgLevel::Info, "VFD Queue Full");
        }

        return rpm;
    }

    void VFD::stop() { set_mode(SpindleState::Disable, false); }

    // state is cached rather than read right now to prevent delays
    SpindleState VFD::get_state() { return _current_state; }

    // Calculate the CRC on all of the byte except the last 2
    // It then added the CRC to those last 2 bytes
    // full_msg_len This is the length of the message including the 2 crc bytes
    // Source: https://ctlsys.com/support/how_to_compute_the_modbus_rtu_message_crc/
    uint16_t VFD::ModRTU_CRC(uint8_t* buf, int msg_len) {
        uint16_t crc = 0xFFFF;
        for (int pos = 0; pos < msg_len; pos++) {
            crc ^= uint16_t(buf[pos]);  // XOR byte into least sig. byte of crc.

            for (int i = 8; i != 0; i--) {  // Loop over each bit
                if ((crc & 0x0001) != 0) {  // If the LSB is set
                    crc >>= 1;              // Shift right and XOR 0xA001
                    crc ^= 0xA001;
                } else {        // Else LSB is not set
                    crc >>= 1;  // Just shift right
                }
            }
        }

        return crc;
    }
}
