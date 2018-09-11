/*
  report.c - reporting and messaging methods
  Part of Grbl  

  Copyright (c) 2012-2016 Sungeun K. Jeon for Gnea Research LLC     
		 
	2018 -	Bart Dring This file was modifed for use on the ESP32
					CPU. Do not use this with Grbl for atMega328P

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
*/

/*
  This file functions as the primary feedback interface for Grbl. Any outgoing data, such
  as the protocol status messages, feedback messages, and status reports, are stored here.
  For the most part, these functions primarily are called from protocol.c methods. If a
  different style feedback is desired (i.e. JSON), then a user can change these following
  methods to accomodate their needs.
	
	
	ESP32 Notes:
	
	Major rewrite to fix issues with BlueTooth. As described here there is a
	when you try to send data a single byte at a time using SerialBT.write(...).
	https://github.com/espressif/arduino-esp32/issues/1537
	
	A solution is to send messages as a string using SerialBT.print(...). Use
	a short delay after each send. Therefore this file needed to be rewritten 
	to work that way. AVR Grbl was written to be super efficient to give it
	good performance. This is far less efficient, but the ESP32 can handle it.
	Do not use this version of the file with AVR Grbl.
	
	ESP32 discussion here ...  https://github.com/bdring/Grbl_Esp32/issues/3
	
	
*/

#include "grbl.h"


// this is a generic send function that everything should use, so interfaces could be added (Bluetooth, etc)
void grbl_send(char *text)
{	
	#ifdef ENABLE_BLUETOOTH
		if (SerialBT.hasClient())
		{
			SerialBT.print(text);
			//delay(10); // possible fix for dropped characters					
		}
	#endif
	
	Serial.print(text);	
}

// This is a formating version of the grbl_send(...) function that work like printf
void grbl_sendf(const char *format, ...)
{
    char loc_buf[64];
    char * temp = loc_buf;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    size_t len = vsnprintf(NULL, 0, format, arg);
    va_end(copy);
    if(len >= sizeof(loc_buf)){
        temp = new char[len+1];
        if(temp == NULL) {
            return;
        }
    }
    len = vsnprintf(temp, len+1, format, arg);
    grbl_send(temp);
    va_end(arg);
    if(len > 64){
        delete[] temp;
    }
}

// formats axis values into a string and returns that string in rpt
static void report_util_axis_values(float *axis_value, char *rpt) {
  uint8_t idx;
	char axisVal[10];
	
	rpt[0] = '\0';
	
  for (idx=0; idx<N_AXIS; idx++) {
		sprintf(axisVal, "%4.3f", axis_value[idx]);
		strcat(rpt, axisVal);
    
    if (idx < (N_AXIS-1)) 
		{ 
			strcat(rpt, ",");
		}
  }
}


void get_state(char *foo)
{        
    // pad them to same length
    switch (sys.state) {
    case STATE_IDLE: strcpy(foo," Idle ");; break;
    case STATE_CYCLE: strcpy(foo," Run  "); break;
    case STATE_HOLD: strcpy(foo," Hold "); break;
    case STATE_HOMING: strcpy(foo," Home "); break;
    case STATE_ALARM: strcpy(foo," Alarm"); break;
    case STATE_CHECK_MODE: strcpy(foo," Check"); break;
    case STATE_SAFETY_DOOR: strcpy(foo," Door "); break;
    default:strcpy(foo,"  ?  "); break;
  }
}

// Handles the primary confirmation protocol response for streaming interfaces and human-feedback.
// For every incoming line, this method responds with an 'ok' for a successful command or an
// 'error:'  to indicate some error event with the line or some critical system error during
// operation. Errors events can originate from the g-code parser, settings module, or asynchronously
// from a critical error, such as a triggered hard limit. Interface should always monitor for these
// responses.
void report_status_message(uint8_t status_code)
{	
  switch(status_code) {
    case STATUS_OK: // STATUS_OK
			#ifdef ENABLE_SD_CARD
				if (SD_file_running)
					SD_ready_next = true; // flag so system_execute_line() will send the next line
				else				
					grbl_send("ok\r\n");
			#else
				grbl_send("ok\r\n");
			#endif					
			break;			
    default:
			grbl_sendf("error:%d\r\n", status_code);
  }
}



// Prints alarm messages.
void report_alarm_message(uint8_t alarm_code)
{	
	grbl_sendf("ALARM:%d\r\n", alarm_code);		
  delay_ms(500); // Force delay to ensure message clears serial write buffer.
}

// Prints feedback messages. This serves as a centralized method to provide additional
// user feedback for things that are not of the status/alarm message protocol. These are
// messages such as setup warnings, switch toggling, and how to exit alarms.
// NOTE: For interfaces, messages are always placed within brackets. And if silent mode
// is installed, the message number codes are less than zero.
void report_feedback_message(uint8_t message_code)
{
	switch(message_code) {
    case MESSAGE_CRITICAL_EVENT:
      grbl_sendf("[MSG:%s]\r\n", "Reset to continue"); break;
    case MESSAGE_ALARM_LOCK:
      grbl_sendf("[MSG:%s]\r\n", "'$H'|'$X' to unlock"); break;
    case MESSAGE_ALARM_UNLOCK:
      grbl_sendf("[MSG:%s]\r\n", "Caution: Unlocked"); break;
    case MESSAGE_ENABLED:
      grbl_sendf("[MSG:%s]\r\n", "Enabled"); break;
    case MESSAGE_DISABLED:
      grbl_sendf("[MSG:%s]\r\n", "Disabled"); break;
    case MESSAGE_SAFETY_DOOR_AJAR:
      grbl_sendf("[MSG:%s]\r\n", "Check Door"); break;
    case MESSAGE_CHECK_LIMITS:
      grbl_sendf("[MSG:%s]\r\n", "Check Limits"); break;
    case MESSAGE_PROGRAM_END:
      grbl_sendf("[MSG:%s]\r\n", "Pgm End"); break;
    case MESSAGE_RESTORE_DEFAULTS:
      grbl_sendf("[MSG:%s]\r\n", "Restoring defaults"); break;
    case MESSAGE_SPINDLE_RESTORE:
      grbl_sendf("[MSG:%s]\r\n", "Restoring spindle"); break;
    case MESSAGE_SLEEP_MODE:
      grbl_sendf("[MSG:%s]\r\n", "Sleeping"); break;
  }  		
}


// Welcome message
void report_init_message()
{
	grbl_send("\r\nGrbl " GRBL_VERSION " ['$' for help]\r\n");
}

// Grbl help message
void report_grbl_help() {	
  grbl_send("[HLP:$$ $# $G $I $N $x=val $Nx=line $J=line $SLP $C $X $H $F ~ ! ? ctrl-x]\r\n"); 
	#ifdef VERBOSE_HELP
		#ifdef ENABLE_BLUETOOTH
			return; // to much data for BT until fixed
		#endif		
		settings_help();
	#endif
}


// Grbl global settings print out.
// NOTE: The numbering scheme here must correlate to storing in settings.c
void report_grbl_settings() {
  // Print Grbl settings.
	char setting[20];
	char rpt[800];
	
	rpt[0] = '\0';
	
	sprintf(setting, "$0=%d\r\n", settings.pulse_microseconds); strcat(rpt, setting);	
	sprintf(setting, "$1=%d\r\n", settings.stepper_idle_lock_time);  strcat(rpt, setting);
	sprintf(setting, "$2=%d\r\n", settings.step_invert_mask);  strcat(rpt, setting);
	sprintf(setting, "$3=%d\r\n", settings.dir_invert_mask);  strcat(rpt, setting);
	sprintf(setting, "$4=%d\r\n", bit_istrue(settings.flags,BITFLAG_INVERT_ST_ENABLE));  strcat(rpt, setting);
	sprintf(setting, "$5=%d\r\n", bit_istrue(settings.flags,BITFLAG_INVERT_LIMIT_PINS));  strcat(rpt, setting);
	sprintf(setting, "$6=%d\r\n", bit_istrue(settings.flags,BITFLAG_INVERT_PROBE_PIN));  strcat(rpt, setting);
	sprintf(setting, "$10=%d\r\n", settings.status_report_mask);  strcat(rpt, setting);
	
	sprintf(setting, "$11=%4.3f\r\n", settings.junction_deviation);   strcat(rpt, setting);	
	sprintf(setting, "$12=%4.3f\r\n", settings.arc_tolerance);   strcat(rpt, setting);	
	
  sprintf(setting, "$13=%d\r\n", bit_istrue(settings.flags,BITFLAG_REPORT_INCHES));   strcat(rpt, setting);	
	sprintf(setting, "$20=%d\r\n", bit_istrue(settings.flags,BITFLAG_SOFT_LIMIT_ENABLE));   strcat(rpt, setting);	
	sprintf(setting, "$21=%d\r\n", bit_istrue(settings.flags,BITFLAG_HARD_LIMIT_ENABLE));   strcat(rpt, setting);	
	sprintf(setting, "$22=%d\r\n", bit_istrue(settings.flags,BITFLAG_HOMING_ENABLE));   strcat(rpt, setting);	
	sprintf(setting, "$23=%d\r\n", settings.homing_dir_mask);   strcat(rpt, setting);	
	
	sprintf(setting, "$24=%4.3f\r\n", settings.homing_feed_rate);   strcat(rpt, setting);	
	sprintf(setting, "$25=%4.3f\r\n", settings.homing_seek_rate);   strcat(rpt, setting);	

	sprintf(setting, "$26=%d\r\n", settings.homing_debounce_delay);   strcat(rpt, setting);	
  
	sprintf(setting, "$27=%4.3f\r\n", settings.homing_pulloff);   strcat(rpt, setting);	
	sprintf(setting, "$30=%4.3f\r\n", settings.rpm_max);   strcat(rpt, setting);	
	sprintf(setting, "$31=%4.3f\r\n", settings.rpm_min);   strcat(rpt, setting);	
	 
  #ifdef VARIABLE_SPINDLE
		sprintf(setting, "$32=%d\r\n", bit_istrue(settings.flags,BITFLAG_LASER_MODE));  strcat(rpt, setting);	
  #else
    strcat(rpt, "$32=0\r\n");
  #endif
	
  // Print axis settings
  uint8_t idx, set_idx;
  uint8_t val = AXIS_SETTINGS_START_VAL;
  for (set_idx=0; set_idx<AXIS_N_SETTINGS; set_idx++) {
    for (idx=0; idx<N_AXIS; idx++) {
      switch (set_idx) {
				case 0: sprintf(setting, "$%d=%4.3f\r\n", val+idx, settings.steps_per_mm[idx]);   strcat(rpt, setting);	 break;
				case 1: sprintf(setting, "$%d=%4.3f\r\n", val+idx, settings.max_rate[idx]);   strcat(rpt, setting);	 break;
				case 2: sprintf(setting, "$%d=%4.3f\r\n", val+idx, settings.acceleration[idx]/(60*60));   strcat(rpt, setting);	 break;
				case 3: sprintf(setting, "$%d=%4.3f\r\n", val+idx, -settings.max_travel[idx]);   strcat(rpt, setting);	 break;		        
      }
    }
    val += AXIS_SETTINGS_INCREMENT;
  }
	
	grbl_send(rpt);
}






// Prints current probe parameters. Upon a probe command, these parameters are updated upon a
// successful probe or upon a failed probe with the G38.3 without errors command (if supported).
// These values are retained until Grbl is power-cycled, whereby they will be re-zeroed.
void report_probe_parameters()
{
  // Report in terms of machine position.	
	float print_position[N_AXIS];
	char probe_rpt[50];	// the probe report we are building here
	char temp[30];
	
	strcpy(probe_rpt, "[PRB:"); // initialize the string with the first characters
  
	// get the machine position and put them into a string and append to the probe report
  system_convert_array_steps_to_mpos(print_position,sys_probe_position);
	report_util_axis_values(print_position, temp);	
	strcat(probe_rpt, temp);
	
	// add the success indicator and add closing characters
	sprintf(temp, ":%d]\r\n", sys.probe_succeeded);	
	strcat(probe_rpt, temp);
	
	grbl_send(probe_rpt); // send the report 
}




// Prints Grbl NGC parameters (coordinate offsets, probing)
void report_ngc_parameters()
{
  float coord_data[N_AXIS];
  uint8_t coord_select;
	char temp[50];
	char ngc_rpt[400];	
	
	ngc_rpt[0] = '\0';	
	
  for (coord_select = 0; coord_select <= SETTING_INDEX_NCOORD; coord_select++) {
    if (!(settings_read_coord_data(coord_select,coord_data))) {
      report_status_message(STATUS_SETTING_READ_FAIL);
      return;
    }
		strcat(ngc_rpt, "[G");    
    switch (coord_select) {
      case 6: strcat(ngc_rpt, "28"); break;
      case 7: strcat(ngc_rpt, "30"); break;
      default: 
			  sprintf(temp, "%d", coord_select+54);
				strcat(ngc_rpt, temp);
				break; // G54-G59
    }    
		strcat(ngc_rpt, ":");		
    report_util_axis_values(coord_data, temp);
		strcat(ngc_rpt, temp);
	  strcat(ngc_rpt, "]\r\n");    
  }
		
	strcat(ngc_rpt, "[G92:"); // Print G92,G92.1 which are not persistent in memory
  report_util_axis_values(gc_state.coord_offset, temp);
	strcat(ngc_rpt, temp);
	strcat(ngc_rpt, "]\r\n");
  strcat(ngc_rpt, "[TLO:"); // Print tool length offset value
	
	if (bit_istrue(settings.flags,BITFLAG_REPORT_INCHES)) {
		sprintf(temp, "%4.3f]\r\n", gc_state.tool_length_offset * INCH_PER_MM);
  } else {
    sprintf(temp, "%4.3f]\r\n", gc_state.tool_length_offset);
  }
	strcat(ngc_rpt, temp);
	
	grbl_send(ngc_rpt);
	
  report_probe_parameters(); // TDO ======================= Print probe parameters. Not persistent in memory.
}



// Print current gcode parser mode state
void report_gcode_modes()
{
	char temp[20];
	char modes_rpt[75];
	
	
  strcpy(modes_rpt, "[GC:G");
	
	
  if (gc_state.modal.motion >= MOTION_MODE_PROBE_TOWARD) {
    sprintf(temp, "38.%d", gc_state.modal.motion - (MOTION_MODE_PROBE_TOWARD-2));		
  } else {
		sprintf(temp, "%d", gc_state.modal.motion);	    
  }
	strcat(modes_rpt, temp);

	sprintf(temp, " G%d", gc_state.modal.coord_select+54);
	strcat(modes_rpt, temp);
	
  sprintf(temp, " G%d", gc_state.modal.plane_select+17);
	strcat(modes_rpt, temp);

  sprintf(temp, " G%d", 21-gc_state.modal.units);
	strcat(modes_rpt, temp);

	sprintf(temp, " G%d", gc_state.modal.distance+90);
	strcat(modes_rpt, temp);

  sprintf(temp, " G%d", 94-gc_state.modal.feed_rate);
	strcat(modes_rpt, temp);

  
  if (gc_state.modal.program_flow) {
    //report_util_gcode_modes_M();
    switch (gc_state.modal.program_flow) {
      case PROGRAM_FLOW_PAUSED : strcat(modes_rpt, " M0"); //serial_write('0'); break;
      // case PROGRAM_FLOW_OPTIONAL_STOP : serial_write('1'); break; // M1 is ignored and not supported.
      case PROGRAM_FLOW_COMPLETED_M2 : 
      case PROGRAM_FLOW_COMPLETED_M30 : 
			  sprintf(temp, " M%d", gc_state.modal.program_flow);
				strcat(modes_rpt, temp);
        break;
    }
  }

  
  switch (gc_state.modal.spindle) {
    case SPINDLE_ENABLE_CW : strcat(modes_rpt, " M3"); break;
    case SPINDLE_ENABLE_CCW : strcat(modes_rpt, " M4"); break;
    case SPINDLE_DISABLE : strcat(modes_rpt, " M5"); break;
  }

  //report_util_gcode_modes_M();
  #ifdef ENABLE_M7
    if (gc_state.modal.coolant) { // Note: Multiple coolant states may be active at the same time.
      if (gc_state.modal.coolant & PL_COND_FLAG_COOLANT_MIST) { strcat(modes_rpt, " M7"); }
      if (gc_state.modal.coolant & PL_COND_FLAG_COOLANT_FLOOD) { strcat(modes_rpt, " M8"); }
    } else { strcat(modes_rpt, " M9"); }
  #else
    if (gc_state.modal.coolant) { strcat(modes_rpt, " M8"); }
    else { strcat(modes_rpt, " M9"); }
  #endif

	sprintf(temp, " T%d", gc_state.tool);
	strcat(modes_rpt, temp); 

  sprintf(temp, " F%4.3f", gc_state.feed_rate);
	strcat(modes_rpt, temp);
	
  #ifdef VARIABLE_SPINDLE    
		sprintf(temp, " S%4.3f", gc_state.spindle_speed);
	  strcat(modes_rpt, temp);
  #endif

  strcat(modes_rpt, "]\r\n");
	
	grbl_send(modes_rpt);
}



// Prints specified startup line
void report_startup_line(uint8_t n, char *line)
{	
	grbl_sendf("$N%d=%s\r\n", n, line);	
}

void report_execute_startup_message(char *line, uint8_t status_code)
{
	char temp[80];
	
	grbl_sendf(">%s:", line);
	
  report_status_message(status_code);    // TODO reduce number of back to back BT sends 
}

// Prints build info line
void report_build_info(char *line)
{
	char build_info[50];
	
	strcpy(build_info, "[VER:" GRBL_VERSION "." GRBL_VERSION_BUILD ":");
	strcat(build_info, line);
	strcat(build_info, "]\r\n[OPT:");
	  
  #ifdef VARIABLE_SPINDLE
    strcat(build_info,"V");
  #endif
  #ifdef USE_LINE_NUMBERS
    strcat(build_info,"N");
  #endif
  #ifdef ENABLE_M7
    strcat(build_info,"M");
  #endif
  #ifdef COREXY
    strcat(build_info,"C");
  #endif
  #ifdef PARKING_ENABLE
    strcat(build_info,"P");
  #endif
  #ifdef HOMING_FORCE_SET_ORIGIN
    strcat(build_info,"Z");
  #endif
  #ifdef HOMING_SINGLE_AXIS_COMMANDS
    strcat(build_info,"H");
  #endif
  #ifdef LIMITS_TWO_SWITCHES_ON_AXES
    strcat(build_info,"L");
  #endif
  #ifdef ALLOW_FEED_OVERRIDE_DURING_PROBE_CYCLES
    strcat(build_info,"A");
  #endif
	#ifdef ENABLE_BLUETOOTH
		strcat(build_info,"B");
	#endif
	#ifdef ENABLE_SD_CARD
		strcat(build_info,"S");
	#endif
  #ifndef ENABLE_RESTORE_EEPROM_WIPE_ALL // NOTE: Shown when disabled.
    strcat(build_info,"*");
  #endif
  #ifndef ENABLE_RESTORE_EEPROM_DEFAULT_SETTINGS // NOTE: Shown when disabled. 
    strcat(build_info,"$");
  #endif
  #ifndef ENABLE_RESTORE_EEPROM_CLEAR_PARAMETERS // NOTE: Shown when disabled.
    strcat(build_info,"#");
  #endif
  #ifndef ENABLE_BUILD_INFO_WRITE_COMMAND // NOTE: Shown when disabled.
    strcat(build_info,"I");
  #endif
  #ifndef FORCE_BUFFER_SYNC_DURING_EEPROM_WRITE // NOTE: Shown when disabled.
    strcat(build_info,"E");
  #endif
  #ifndef FORCE_BUFFER_SYNC_DURING_WCO_CHANGE // NOTE: Shown when disabled.
    strcat(build_info,"W");
  #endif
  // NOTE: Compiled values, like override increments/max/min values, may be added at some point later.
  // These will likely have a comma delimiter to separate them.   
  
  strcat(build_info,"]\r\n");
  grbl_send(build_info); 
}




// Prints the character string line Grbl has received from the user, which has been pre-parsed,
// and has been sent into protocol_execute_line() routine to be executed by Grbl.
void report_echo_line_received(char *line)
{
	char temp[80];
	
	sprintf(temp, "[echo: %s]\r\n", line);
	
}

//--------------------------------------------- Converted up to here ---------------------------------------------------------------------


 // Prints real-time data. This function grabs a real-time snapshot of the stepper subprogram
 // and the actual location of the CNC machine. Users may change the following function to their
 // specific needs, but the desired real-time data report must be as short as possible. This is
 // requires as it minimizes the computational overhead and allows grbl to keep running smoothly,
 // especially during g-code programs with fast, short line segments and high frequency reports (5-20Hz).
void report_realtime_status()
{
  uint8_t idx;
  int32_t current_position[N_AXIS]; // Copy current state of the system position variable
  memcpy(current_position,sys_position,sizeof(sys_position));
  float print_position[N_AXIS];
	
	char status[200];
	char temp[50];
	
  system_convert_array_steps_to_mpos(print_position,current_position);

  // Report current machine state and sub-states  
	strcpy(status, "<");
  switch (sys.state) {
    case STATE_IDLE: strcat(status, "Idle"); break;
    case STATE_CYCLE: strcat(status, "Run"); break;
    case STATE_HOLD:
    
      if (!(sys.suspend & SUSPEND_JOG_CANCEL)) {
        strcat(status, "Hold:");
        if (sys.suspend & SUSPEND_HOLD_COMPLETE) { strcat(status, "0"); } // Ready to resume
        else { strcat(status, "1"); } // Actively holding
        break;
      } // Continues to print jog state during jog cancel.
    case STATE_JOG: strcat(status, "Jog"); break;
    case STATE_HOMING: strcat(status, "Home"); break;
    case STATE_ALARM: strcat(status, "Alarm"); break;
    case STATE_CHECK_MODE: strcat(status, "Check"); break;
    case STATE_SAFETY_DOOR:
      strcat(status, "Door:");
      if (sys.suspend & SUSPEND_INITIATE_RESTORE) {
        strcat(status, "3"); // Restoring
      } else {
        if (sys.suspend & SUSPEND_RETRACT_COMPLETE) {
          if (sys.suspend & SUSPEND_SAFETY_DOOR_AJAR) {
            strcat(status, "1"); // Door ajar
          } else {
            strcat(status, "0");
          } // Door closed and ready to resume
        } else {
          strcat(status, "2"); // Retracting
        }
      }
      break;
    case STATE_SLEEP: strcat(status, "Sleep"); break;
  }

  float wco[N_AXIS];
  if (bit_isfalse(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE) ||
      (sys.report_wco_counter == 0) ) {
    for (idx=0; idx< N_AXIS; idx++) {
      // Apply work coordinate offsets and tool length offset to current position.
      wco[idx] = gc_state.coord_system[idx]+gc_state.coord_offset[idx];
      if (idx == TOOL_LENGTH_OFFSET_AXIS) { wco[idx] += gc_state.tool_length_offset; }
      if (bit_isfalse(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE)) {
        print_position[idx] -= wco[idx];
      }
    }
  }

  // Report machine position
  if (bit_istrue(settings.status_report_mask,BITFLAG_RT_STATUS_POSITION_TYPE)) {
    strcat(status, "|MPos:");
  } else {
    strcat(status, "|WPos:");
  }
  report_util_axis_values(print_position, temp);
	strcat(status, temp);

  // Returns planner and serial read buffer states.
  #ifdef REPORT_FIELD_BUFFER_STATE
    if (bit_istrue(settings.status_report_mask,BITFLAG_RT_STATUS_BUFFER_STATE)) {      
			sprintf(temp, "|Bf:%d,%d", plan_get_block_buffer_available(), serial_get_rx_buffer_available());
			strcat(status, temp);
    }
  #endif

  #ifdef USE_LINE_NUMBERS
    #ifdef REPORT_FIELD_LINE_NUMBERS
      // Report current line number
      plan_block_t * cur_block = plan_get_current_block();
      if (cur_block != NULL) {
        uint32_t ln = cur_block->line_number;
        if (ln > 0) {
					sprintf(temp, "|Ln:%d", ln);
					strcat(status, temp);
        }
      }
    #endif
  #endif

  // Report realtime feed speed
  #ifdef REPORT_FIELD_CURRENT_FEED_SPEED
    #ifdef VARIABLE_SPINDLE      
			sprintf(temp, "|FS:%4.3f,%4.3f", st_get_realtime_rate(), sys.spindle_speed);
			strcat(status, temp);
    #else
      
			sprintf(temp, "|F:%4.3f", st_get_realtime_rate());
			strcat(status, temp);
    #endif      
  #endif

  #ifdef REPORT_FIELD_PIN_STATE
    uint8_t lim_pin_state = limits_get_state();
    uint8_t ctrl_pin_state = system_control_get_state();
    uint8_t prb_pin_state = probe_get_state();
    if (lim_pin_state | ctrl_pin_state | prb_pin_state) {      
			strcat(status, "|Pn:");
      if (prb_pin_state) { strcat(status, "P"); }
      if (lim_pin_state) {
        if (bit_istrue(lim_pin_state,bit(X_AXIS))) { strcat(status, "X"); }
        if (bit_istrue(lim_pin_state,bit(Y_AXIS))) { strcat(status, "Y"); }
        if (bit_istrue(lim_pin_state,bit(Z_AXIS))) { strcat(status, "Z"); }
        #ifdef A_AXIS
         if (bit_istrue(lim_pin_state,bit(A_AXIS))) { strcat(status, "A"); }
        #endif
        #ifdef B_AXIS
         if (bit_istrue(lim_pin_state,bit(B_AXIS))) { strcat(status, "B"); }
        #endif
        #ifdef C_AXIS
         if (bit_istrue(lim_pin_state,bit(C_AXIS))) { strcat(status, "C"); }
        #endif
      }
      if (ctrl_pin_state) {
        #ifdef ENABLE_SAFETY_DOOR_INPUT_PIN
          if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_SAFETY_DOOR)) { strcat(status, "D"); }
        #endif
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_RESET)) { strcat(status, "R"); }
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_FEED_HOLD)) { strcat(status, "H"); }
        if (bit_istrue(ctrl_pin_state,CONTROL_PIN_INDEX_CYCLE_START)) { strcat(status, "S"); }
      }
    }
  #endif

  #ifdef REPORT_FIELD_WORK_COORD_OFFSET
    if (sys.report_wco_counter > 0) { sys.report_wco_counter--; }
    else {
      if (sys.state & (STATE_HOMING | STATE_CYCLE | STATE_HOLD | STATE_JOG | STATE_SAFETY_DOOR)) {
        sys.report_wco_counter = (REPORT_WCO_REFRESH_BUSY_COUNT-1); // Reset counter for slow refresh
      } else { sys.report_wco_counter = (REPORT_WCO_REFRESH_IDLE_COUNT-1); }
      if (sys.report_ovr_counter == 0) { sys.report_ovr_counter = 1; } // Set override on next report.
      strcat(status, "|WCO:");
      report_util_axis_values(wco, temp);
			strcat(status, temp);
    }
  #endif

  #ifdef REPORT_FIELD_OVERRIDES
    if (sys.report_ovr_counter > 0) { sys.report_ovr_counter--; }
    else {
      if (sys.state & (STATE_HOMING | STATE_CYCLE | STATE_HOLD | STATE_JOG | STATE_SAFETY_DOOR)) {
        sys.report_ovr_counter = (REPORT_OVR_REFRESH_BUSY_COUNT-1); // Reset counter for slow refresh
      } else { sys.report_ovr_counter = (REPORT_OVR_REFRESH_IDLE_COUNT-1); }      
			sprintf(temp, "|Ov:%d,%d,%d", sys.f_override, sys.r_override, sys.spindle_speed_ovr);
			strcat(status, temp);
      

      uint8_t sp_state = spindle_get_state();
      uint8_t cl_state = coolant_get_state();
      if (sp_state || cl_state) {
        strcat(status, "|A:");
        if (sp_state) { // != SPINDLE_STATE_DISABLE
          #ifdef VARIABLE_SPINDLE 
            #ifdef USE_SPINDLE_DIR_AS_ENABLE_PIN
              strcat(status, "S"); // CW
            #else
              if (sp_state == SPINDLE_STATE_CW) { strcat(status, "S"); } // CW
              else { strcat(status, "C"); } // CCW
            #endif
          #else
            if (sp_state & SPINDLE_STATE_CW) { strcat(status, "S"); } // CW
            else { strcat(status, "C"); } // CCW
          #endif
        }
        if (cl_state & COOLANT_STATE_FLOOD) { strcat(status, "F"); }
        #ifdef ENABLE_M7
          if (cl_state & COOLANT_STATE_MIST) { strcat(status, "M"); }
        #endif
      }  
    }
  #endif
	
	#ifdef ENABLE_SD_CARD
		if (SD_file_running) {
			sprintf(temp, "|SD:%4.2f", sd_report_perc_complete());
			strcat(status, temp);
		}
	#endif

  strcat(status, ">\r\n");
	
	grbl_send(status);	
}

void report_realtime_steps()
{
	uint8_t idx;
	for (idx=0; idx< N_AXIS; idx++) {
		Serial.println(sys_position[idx]);
	}
}

void settings_help()
{
	Serial.println("[HLP ----------- Setting Descriptions -----------]");
	Serial.println("[HLP $0=Step Pulse Delay (3-255)]");
	Serial.println("[HLP $1=Step Idle Delay (0-254, 255 keeps motors on)]");
	Serial.println("[HLP $2=Step Pulse Invert Mask(00000ZYZ)]");
	Serial.println("[HLP $3=Direction Invert Mask(00000XYZ)]");
	Serial.println("[HLP $4=Step Enable Invert (boolean)]");
	Serial.println("[HLP $6=Invert Probe Pin (boolean)]");
	Serial.println("[HLP $10Status Report Options (See Wiki)]");
	Serial.println("[HLP $11=Junction Deviation (float millimeters)]");
	Serial.println("[HLP $12=Arc Tolerance (float millimeters)]");
	Serial.println("[HLP $13=Report in Inches (boolean)]");
	Serial.println("[HLP $20=Soft Limits Enable (boolean)]");
	Serial.println("[HLP $21=Hard Limits Enable (boolean)]");
	Serial.println("[HLP $22=Homing Enable (boolean)]");
	Serial.println("[HLP $23=Homing Direction Invert (00000ZYX)]");
	Serial.println("[HLP $24=Homing Feed Rate (mm/min)]");
	Serial.println("[HLP $25=Homing Seek Rate (mm/min)]");
	Serial.println("[HLP $26=Homing Switch Debounce Delay (milliseconds)]");
	Serial.println("[HLP $27=Homing Switch Pull-off Distance (millimeters)]");
	Serial.println("[HLP $30=Max Spindle Speed (RPM)]");
	Serial.println("[HLP $31=Min Spindle Speed (RPM)]");
	Serial.println("[HLP $32=Laser Mode Enable (boolean)]");
	Serial.println("[HLP $100-102= XYZ Axis Resolution (step/mm)]");
	Serial.println("[HLP $110-112= XYZ Axis Max Rate (mm/min)]");
	Serial.println("[HLP $120-122= XYZ Axis Acceleration (step/mm^2)]");
	Serial.println("[HLP $130-132= XYZ Axis max Travel (step/mm)]");
	
}

#ifdef DEBUG
  void report_realtime_debug()
  {

  }
#endif

