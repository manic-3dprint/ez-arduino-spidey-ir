/* -----------------------------------------------------------------------------
  - Project: Remote control Crawling robot
  - Author:  panerqiang@sunfounder.com
  - Date:  2015/1/27
   -----------------------------------------------------------------------------
  - Overview
  - This project was written for the Crawling robot desigened by Sunfounder.
  https://github.com/sunfounder/SunFounder_Crawling_Quadruped_Robot_Kit_for_Arduino

  modified by Regis https://www.thingiverse.com/thing:2204279
  modified by John Crombie https://www.thingiverse.com/thing:2825097
  modified by manic-3d-print https://www.thingiverse.com/thing:3099712
  ---------------------------------------------------------------------------*/



/* Includes ------------------------------------------------------------------*/
#include <setjmp.h>
#include <EEPROM.h>
#include <Servo.h>
#include "IRremote.h"
#include "IRcontrolKeys.h"

#define EEPROM_MAGIC  0xabcd
#define EEPROM_OFFSET 2   //eeprom starting offset to store servo offset of calibration
/* Servos --------------------------------------------------------------------*/
//define 12 servos for 4 legs
Servo servo[4][3];
//define servos' ports
const int servo_pin[4][3] = { {2, 3, 4}, {5, 6, 7}, {8, 9, 10}, {11, 12, 13} };
int servo_error[4][3] = { {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0} }; // angle trim offset for servo derivation in polar_to_servo
/* Size of the robot ---------------------------------------------------------*/
const int femur_servo_index = 0;
const int tibia_servo_index = 1;
const int coxa_servo_index = 2;
const float femur = 55;
const float tibia = 81.5;
const float coxa = 33;
const float length_side = 65.5;
const float z_absolute = -20;
/* Constants for movement ----------------------------------------------------*/
const float z_default = -50, z_up = -20, z_boot = z_absolute, z_bow_relative = 25, z_tall = -100;
const float x_default = 62, x_offset = 0;
const float y_start = 0, y_step = 40;
const float sprawl_leg = 95; // used for both X and Y on sprawl movement
const int WALK = 0;
const int RELAX = 1;
const int SPRAWL = 2;
const bool FALSE = 0;
const bool TRUE = 1;
/* variables for movement ----------------------------------------------------*/
volatile float site_now[4][3];    //real-time coordinates of the end of each leg
volatile float site_expect[4][3]; //expected coordinates of the end of each leg
volatile bool capture = FALSE;
float temp_speed[4][3];   //each axis' speed, needs to be recalculated before each movement
float move_speed;     //movement speed
float speed_multiple = 1; //movement speed multiple
int leg_position; // one of WALK, RELAX, SPRAWL
const float spot_turn_speed = 4;
const float leg_move_speed = 8;
const float body_move_speed = 3;
const float stand_seat_speed = 1;
const float gyrate_speed = 0.8;
const float bow_speed = 1;
volatile int rest_counter;      //+1/0.02s, for automatic rest
//functions' parameter
const float KEEP = 255;
//define PI for calculation
const float pi = 3.1415926;
volatile float site_cg[4][3]; // record current position to determine how to move cg
/* Constants for turn --------------------------------------------------------*/
//temp length
const float temp_a = sqrt(pow(2 * x_default + length_side, 2) + pow(y_step, 2));
const float temp_b = 2 * (y_start + y_step) + length_side;
const float temp_c = sqrt(pow(2 * x_default + length_side, 2) + pow(2 * y_start + y_step + length_side, 2));
const float temp_alpha = acos((pow(temp_a, 2) + pow(temp_b, 2) - pow(temp_c, 2)) / 2 / temp_a / temp_b);
//site for turn
const float turn_x1 = (temp_a - length_side) / 2;
const float turn_y1 = y_start + y_step / 2;
const float turn_x0 = turn_x1 - temp_b * cos(temp_alpha);
const float turn_y0 = temp_b * sin(temp_alpha) - turn_y1 - length_side;
//
const float half_step = 10; // forward and backward
const float cg_shift = 14; // distance to move CG to retain balance with leg raised

/* goBLE----------------------------------------------------------------------*/
#define TIME_INTERVAL 5000

#define FORWARD 'f'
#define LEFT 'l'
#define STAND 's'
#define RIGHT 'r'
#define BACKWARD 'b'
#define BODY_FR_BR 'z'
#define BODY_SI_SI 'x'
#define BODY_SI_SI_DOWN 'y'
#define GYRATE 'i'
#define TALL 't'
#define BOW 'w'
#define STAGGER 'u'
#define HAND_WAV 'h'
#define HAND_SHAKE 'k'
#define SIT 'o'
#define SPRAWL 'p'
#define RELAX_LEG '?'

volatile char cmd = STAND;
volatile bool stop_serial = false;
bool auto_mode = true;
bool random_walk = false;
unsigned long cur_time;
jmp_buf jump_env;


IRrecv irrecv(A0);
decode_results result;

/*
  - setup function
   ---------------------------------------------------------------------------*/
void setup()
{

  //start serial for debug
  Serial.begin(115200); // baud rate chosen for bluetooth compatability
  //
  int val = EEPROMReadWord(0);
  if (val != EEPROM_MAGIC) {
    EEPROMWriteWord(0, EEPROM_MAGIC);
    storeTrim();
  } else {
    loadTrim();
  }
  irrecv.enableIRIn();
  Serial.println("Enabled IRin");
  Serial.println("Robot starts initialization");
  //initialize default parameter
  set_site(0, x_default - x_offset, y_start + y_step, z_boot);
  set_site(1, x_default - x_offset, y_start + y_step, z_boot);
  set_site(2, x_default + x_offset, y_start, z_boot);
  set_site(3, x_default + x_offset, y_start, z_boot);

  leg_position = WALK;

  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 3; j++)
    {
      site_now[i][j] = site_expect[i][j];
      servo[i][j].attach(servo_pin[i][j]);
    }
  }

  calib_servo();
  cli();
  OCR0A = 0xAF;
  TIMSK0 |= _BV(OCIE0A);
  sei();
  Serial.println("Robot initialization Complete");
  cur_time = millis();
  stand();
}

/*
  - loop function
   ---------------------------------------------------------------------------*/
void loop()
{
  int rc = setjmp(jump_env);
  //  if (rc != 0) Serial.println("jumped from delay()");
  gaits();

}

#define CAL_TRIGGER_PIN A5
void calib_servo(void)
{
  pinMode(CAL_TRIGGER_PIN, OUTPUT);
  digitalWrite(CAL_TRIGGER_PIN, 0);
  pinMode(CAL_TRIGGER_PIN, INPUT);
  if (digitalRead(CAL_TRIGGER_PIN)) {
    for (int i = 0; i < 4; i++) {
      servo[i][femur_servo_index].write(90 + servo_error[i][femur_servo_index]);
      servo[i][tibia_servo_index].write(90 + servo_error[i][tibia_servo_index]);
      servo[i][coxa_servo_index].write(90 + servo_error[i][coxa_servo_index]);
    }
    while (digitalRead(CAL_TRIGGER_PIN)) delay(1000);
  }
}

boolean check_ir() {
  bool rc = false;
  unsigned long value = 0;
  static char prev_cmd = '.';
  static long ir_time = millis();
 

  if (irrecv.decode(&result)) {
    value = result.value;
    irrecv.resume();
    ir_time = millis();
    //Serial.println(value, HEX);
    switch (value) {
      case ir_control_keys[KEY_AUTO]: 
            auto_mode = true; 
            goto __auto; 
            break;
      case ir_control_keys[KEY_CONTROL]: 
            auto_mode = false; 
            cmd = STAND;  
            break; 
      case 0xFFFFFFFF: ; break;
    }
    if (!auto_mode) {
      switch (value) {
        case ir_control_keys[KEY_FORWARD]: 
              cmd = FORWARD; break;
        case ir_control_keys[KEY_BACKWARD]: 
              cmd = BACKWARD; break;
        case ir_control_keys[KEY_RIGHT]: 
              cmd = RIGHT; break; 
        case ir_control_keys[KEY_LEFT]: 
              cmd = LEFT; break; 
        case ir_control_keys[KEY_STAND]: 
              cmd = STAND; break; 
        case ir_control_keys[KEY_0]: 
              cmd=RELAX_LEG; break; 
        case ir_control_keys[KEY_1]: 
              cmd = BOW; break; 
        case ir_control_keys[KEY_2]: 
              cmd = HAND_SHAKE; break; 
        case ir_control_keys[KEY_3]: 
               cmd = SPRAWL; break; 
        case ir_control_keys[KEY_4]: 
              cmd = BODY_FR_BR; break; 
        case ir_control_keys[KEY_5]: 
              cmd = TALL; break; 
        case ir_control_keys[KEY_6]: 
              cmd = BODY_SI_SI_DOWN; break; 
        case ir_control_keys[KEY_7]: 
              cmd = GYRATE; break; 
        case ir_control_keys[KEY_8]: 
              cmd = BODY_SI_SI; break; 
        case ir_control_keys[KEY_9]: 
              cmd = HAND_WAV; break; 
      }
    }
  }
  if (!auto_mode && cmd != STAND && (millis() >= ir_time + 500)) {
    cmd = STAND;
    ir_time = millis();
    //Serial.print(value, HEX);
    //Serial.println(" time out");
  }

  //Serial.println(cmd);
  if (auto_mode) {
    static char movements[] = {
      FORWARD, LEFT,
      GYRATE, BODY_FR_BR, BODY_SI_SI, BODY_SI_SI_DOWN,
      TALL, BOW, STAGGER, HAND_WAV, HAND_SHAKE, SPRAWL,
      RIGHT, BACKWARD, SIT, STAND
    };
    static unsigned long old_time = cur_time;
    static int c = 0;
    if (cur_time - old_time >= TIME_INTERVAL ) {
      old_time = cur_time;
__auto:
      if (!random_walk) {
        c = c % (sizeof(movements) / sizeof(char));
        cmd = movements[c++];
      } else {
        c = (int)random(0, sizeof(movements) / sizeof(char));
        cmd = movements[c];
      }
    }
  }
  return rc;
}

boolean  gaits() {
  bool taken = true;

  //Serial.println("cmd " + String(cmd));

  switch (cmd) {
    case FORWARD:
      step_forward(1);
      break;
    case BACKWARD:
      step_back(1);
      break;
    case RIGHT:
      turn_right(1);
      break;
    case LEFT:
      turn_left(1);
      break;
    case STAND:
      stand();
      break;
    case BODY_FR_BR:
      body_forward_backward(1);
      break;
    case BODY_SI_SI:
      body_side_to_side(1);
      break;
    case BODY_SI_SI_DOWN:
      body_side_up_side_down(1);
      break;
    case GYRATE:
      gyrate(1);
      break;
    case BOW:
      bow(1);
      break;
    case SPRAWL:
      sprawl();
      delay(1000);
    case RELAX_LEG:
      relax_legs();
      delay(2000);
      break;
    case TALL:
      tall(2000);
      break;
    case STAGGER:
      stagger(1);
      break;
    case HAND_WAV:
      hand_wave(1);
      break;
    case HAND_SHAKE:
      hand_shake(1);
      break;
    case SIT:
      sit();
      break;
    default:
      taken = false;
  }
  return taken;
}


void loop2()
{
  //leg position test
  /*  relax_legs();
    set_site(0, KEEP, KEEP, z_default - z_bow_relative);
    set_site(1, KEEP, KEEP, z_default - z_bow_relative);
    set_site(2, KEEP, KEEP, z_default - z_bow_relative);
    set_site(3, KEEP, KEEP, z_default - z_bow_relative);
  */

  Serial.println("Stand");
  stand();
  delay(2000);
  // JC added moves
  Serial.println("Relaxed Pose");
  relax_legs();
  delay(2000);
  Serial.println("Body move Forward and Backwards");
  body_forward_backward(4);
  delay(2000);
  Serial.println("Body move side to side");
  body_side_to_side(4);
  delay(2000);
  Serial.println("Sprawl");
  sprawl();
  delay(2000);
  Serial.println("Gyrate");
  gyrate(3);
  delay(2000);
  Serial.println("Front bow");
  bow(1);
  delay(2000);
  //Serial.println("Back bow");
  //back_bow(5);
  //  delay(2000);
  Serial.println("Tall");
  tall(2000); // raise up. delay and return to normal height
  relax_legs();
  delay(2000);
  Serial.println("Body Twist");
  body_twist(3);
  delay(2000);
  Serial.println("Move Body side up to side down");
  body_side_up_side_down(3);
  delay(2000);
  Serial.println("stagger");
  stagger(3);
  delay(2000);
  // end of JC added moves
  Serial.println("Step forward");
  step_forward(5);
  delay(2000);
  Serial.println("Step back");
  step_back(5);
  delay(2000);
  Serial.println("Turn left");
  turn_left(5);
  delay(2000);
  Serial.println("Turn right");
  turn_right(5);
  delay(2000);
  Serial.println("Hand wave");
  hand_wave(3);
  delay(2000);
  Serial.println("Hand wave");
  hand_shake(3);
  delay(2000);
  Serial.println("Sit");
  sit();
  delay(5000);

}

// Start of JC added move routines
/*
  - JC
  - body gyration movement
  - blocking function
   ---------------------------------------------------------------------------*/
void gyrate(unsigned int number_of_times)
{
  // set leg starting position
  relax_legs();
  move_speed = gyrate_speed;
  //capture = TRUE;
  set_site(0, KEEP, KEEP, z_default - z_bow_relative);
  set_site(3, KEEP, KEEP, z_default + z_bow_relative);
  wait_all_reach();

  while (number_of_times-- > 0)
  {
    set_site(2, KEEP, KEEP, z_default - z_bow_relative);
    set_site(1, KEEP, KEEP, z_default + z_bow_relative);
    set_site(0, KEEP, KEEP, z_default);
    set_site(3, KEEP, KEEP, z_default);
    wait_all_reach();

    set_site(3, KEEP, KEEP, z_default - z_bow_relative);
    set_site(0, KEEP, KEEP, z_default + z_bow_relative);
    set_site(2, KEEP, KEEP, z_default);
    set_site(1, KEEP, KEEP, z_default);
    wait_all_reach();

    set_site(1, KEEP, KEEP, z_default - z_bow_relative);
    set_site(2, KEEP, KEEP, z_default + z_bow_relative);
    set_site(3, KEEP, KEEP, z_default);
    set_site(0, KEEP, KEEP, z_default);
    wait_all_reach();

    set_site(0, KEEP, KEEP, z_default - z_bow_relative);
    set_site(3, KEEP, KEEP, z_default + z_bow_relative);
    set_site(1, KEEP, KEEP, z_default);
    set_site(2, KEEP, KEEP, z_default);
    wait_all_reach();
  }

  // return legs to starting position
  set_site(1, KEEP, KEEP, z_default);
  set_site(3, KEEP, KEEP, z_default);
  capture = FALSE;
  wait_all_reach();
}
/*
  - JC
  - body gyration movement
  - blocking function
   ---------------------------------------------------------------------------*/
void body_side_up_side_down(unsigned int number_of_times)
{
  // set leg starting position
  relax_legs();
  move_speed = gyrate_speed;
  //capture = TRUE;
  set_site(0, KEEP, KEEP, z_default - z_bow_relative);
  set_site(1, KEEP, KEEP, z_default - z_bow_relative);
  set_site(2, KEEP, KEEP, z_default + z_bow_relative);
  set_site(3, KEEP, KEEP, z_default + z_bow_relative);
  wait_all_reach();

  while (number_of_times-- > 0)
  {
    set_site(2, KEEP, KEEP, z_default - z_bow_relative);
    set_site(3, KEEP, KEEP, z_default - z_bow_relative);
    set_site(0, KEEP, KEEP, z_default + z_bow_relative);
    set_site(1, KEEP, KEEP, z_default + z_bow_relative);
    wait_all_reach();

    set_site(0, KEEP, KEEP, z_default - z_bow_relative);
    set_site(1, KEEP, KEEP, z_default - z_bow_relative);
    set_site(2, KEEP, KEEP, z_default + z_bow_relative);
    set_site(3, KEEP, KEEP, z_default + z_bow_relative);
    wait_all_reach();
  }

  // return legs to starting position
  set_site(0, KEEP, KEEP, z_default);
  set_site(1, KEEP, KEEP, z_default);
  set_site(2, KEEP, KEEP, z_default);
  set_site(3, KEEP, KEEP, z_default);
  capture = FALSE;
  wait_all_reach();
}

/*
  -  JC
  - bow front legs
  - blocking function
*/
void bow( unsigned int times)
{
  relax_legs();
  move_speed = bow_speed;
  while (times-- > 0)
  {
    set_site(0, KEEP, KEEP, z_default + z_bow_relative);
    set_site(2, KEEP, KEEP, z_default + z_bow_relative);
    wait_all_reach();
    delay(300);
    set_site(0, KEEP, KEEP, z_default);
    set_site(2, KEEP, KEEP, z_default);
    wait_all_reach();
    delay(300);
  }
}
/*
  -  JC
  - twist body clockwise and counter clockwise
  - blocking function
*/
void body_twist( unsigned int times)
{
  relax_legs();

  move_cg(0);
  set_site(0, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  set_site(0, turn_x1, turn_y1, z_up);
  wait_all_reach();
  return_cg(0);
  set_site(0, turn_x1, turn_y1, z_default);
  wait_all_reach();

  move_cg(1);
  set_site(1, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  set_site(1, turn_x0, turn_y0, z_up);
  wait_all_reach();
  return_cg(1);
  set_site(1, turn_x0, turn_y0, z_default);
  wait_all_reach();

  move_cg(2);
  set_site(2, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  set_site(2, turn_x0, turn_y0, z_up);
  wait_all_reach();
  return_cg(2);
  set_site(2, turn_x0, turn_y0, z_default);
  wait_all_reach();

  move_cg(3);
  set_site(3, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  set_site(3, turn_x1, turn_y1, z_up);
  wait_all_reach();
  return_cg(3);
  set_site(3, turn_x1, turn_y1, z_default);
  wait_all_reach();

  move_speed = bow_speed;
  while (times-- > 0)
  {
    set_site(0, turn_x0, turn_y0, KEEP);
    set_site(1, turn_x1, turn_y1, KEEP);
    set_site(2, turn_x1, turn_y1, KEEP);
    set_site(3, turn_x0, turn_y0, KEEP);
    wait_all_reach();
    delay(300);
    set_site(0, turn_x1, turn_y1, KEEP);
    set_site(1, turn_x0, turn_y0, KEEP);
    set_site(2, turn_x0, turn_y0, KEEP);
    set_site(3, turn_x1, turn_y1, KEEP);
    wait_all_reach();
    delay(300);
  }
  move_speed = leg_move_speed;

  move_cg(0);
  set_site(0, KEEP, KEEP, z_up);
  wait_all_reach();
  set_site(0, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  return_cg(0);
  set_site(0, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();

  move_cg(1);
  set_site(1, KEEP, KEEP, z_up);
  wait_all_reach();
  set_site(1, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  return_cg(1);
  set_site(1, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();

  move_cg(2);
  set_site(2, KEEP, KEEP, z_up);
  wait_all_reach();
  set_site(2, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  return_cg(2);
  set_site(2, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();

  move_cg(3);
  set_site(3, KEEP, KEEP, z_up);
  wait_all_reach();
  set_site(3, x_default - x_offset, y_start + y_step, z_up);
  wait_all_reach();
  return_cg(3);
  set_site(3, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();

}
/*
  -  JC
  - forward and backward body movement based on twist
  - blocking function
*/
void body_forward_backward( unsigned int times)
{
  relax_legs();
  move_speed = bow_speed;
  set_site(0, x_default - x_offset, y_start + y_step - half_step, z_default);
  set_site(1, x_default - x_offset, y_start + y_step + half_step, z_default);
  set_site(2, x_default - x_offset, y_start + y_step - half_step, z_default);
  set_site(3, x_default - x_offset, y_start + y_step + half_step, z_default);
  wait_all_reach();


  while (times-- > 0)
  {
    set_site(0, x_default - x_offset, y_start + y_step + 2 * half_step, z_default);
    set_site(1, x_default - x_offset, y_start + y_step - 2 * half_step, z_default);
    set_site(2, x_default - x_offset, y_start + y_step + 2 * half_step, z_default);
    set_site(3, x_default - x_offset, y_start + y_step - 2 * half_step, z_default);
    wait_all_reach();
    delay(300);
    set_site(0, x_default - x_offset, y_start + y_step - 2 * half_step, z_default);
    set_site(1, x_default - x_offset, y_start + y_step + 2 * half_step, z_default);
    set_site(2, x_default - x_offset, y_start + y_step - 2 * half_step, z_default);
    set_site(3, x_default - x_offset, y_start + y_step + 2 * half_step, z_default);
    wait_all_reach();
    delay(300);
  }
  set_site(0, x_default - x_offset, y_start + y_step, z_default);
  set_site(1, x_default - x_offset, y_start + y_step, z_default);
  set_site(2, x_default - x_offset, y_start + y_step, z_default);
  set_site(3, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();
  move_speed = leg_move_speed;

}
/*
  -  JC
  - forward and backward body movement based on twist
  - blocking function
*/
void body_side_to_side( unsigned int times)
{
  relax_legs();
  move_speed = bow_speed;
  set_site(0, x_default - x_offset - half_step, y_start + y_step, z_default);
  set_site(1, x_default - x_offset - half_step, y_start + y_step, z_default);
  set_site(2, x_default - x_offset + half_step, y_start + y_step, z_default);
  set_site(3, x_default - x_offset + half_step, y_start + y_step, z_default);
  wait_all_reach();


  while (times-- > 0)
  {
    set_site(0, x_default - x_offset + 2 * half_step, y_start + y_step, z_default);
    set_site(1, x_default - x_offset + 2 * half_step, y_start + y_step, z_default);
    set_site(2, x_default - x_offset - 2 * half_step, y_start + y_step, z_default);
    set_site(3, x_default - x_offset - 2 * half_step, y_start + y_step, z_default);
    wait_all_reach();
    delay(300);
    set_site(0, x_default - x_offset - 2 * half_step, y_start + y_step, z_default);
    set_site(1, x_default - x_offset - 2 * half_step, y_start + y_step, z_default);
    set_site(2, x_default - x_offset + 2 * half_step, y_start + y_step, z_default);
    set_site(3, x_default - x_offset + 2 * half_step, y_start + y_step, z_default);
    wait_all_reach();
    delay(300);
  }
  set_site(0, x_default - x_offset, y_start + y_step, z_default);
  set_site(1, x_default - x_offset, y_start + y_step, z_default);
  set_site(2, x_default - x_offset, y_start + y_step, z_default);
  set_site(3, x_default - x_offset, y_start + y_step, z_default);
  wait_all_reach();
  move_speed = leg_move_speed;

}
/*
  -  JC
  - lift_up front legs
  - blocking function
*/
void tall( int time)
{
  relax_legs();
  move_speed = bow_speed;
  set_site(0, KEEP, KEEP, z_tall);
  set_site(2, KEEP, KEEP, z_tall);
  set_site(1, KEEP, KEEP, z_tall);
  set_site(3, KEEP, KEEP, z_tall);
  wait_all_reach();
  delay(time);
  set_site(0, KEEP, KEEP, z_default);
  set_site(2, KEEP, KEEP, z_default);
  set_site(1, KEEP, KEEP, z_default);
  set_site(3, KEEP, KEEP, z_default);
  wait_all_reach();
}
/*
  -  JC
  - bow back legs - looks a bit rude.
  - blocking function
*/
void back_bow( unsigned int times)
{
  relax_legs();
  move_speed = bow_speed;
  while (times-- > 0)
  {
    set_site(1, KEEP, KEEP, z_default - z_bow_relative);
    set_site(3, KEEP, KEEP, z_default - z_bow_relative);
    wait_all_reach();
    set_site(1, KEEP, KEEP, z_default);
    set_site(3, KEEP, KEEP, z_default);
    wait_all_reach();
  }
}
/*
  - JC
  - dance
  - blocking function
   ---------------------------------------------------------------------------*/
void stagger(unsigned int times)
{
  move_speed = leg_move_speed;
  while (times-- > 0)
  {
    walk_legs();
    relax_legs();
    walk_legs_other();
    relax_legs();
  }
  capture = FALSE;
}

/*
  - JC
  - Change_legs to for a X for a relaxed pose
  - blocking function
  ---------------------------------------------------------------------------*/
void relax_legs(void)
{
  move_speed = leg_move_speed;
  switch (leg_position)
  {
    case WALK:
      if (site_now[3][1] == y_start)
      {
        move_cg(2);
        set_site(2, KEEP, KEEP, z_up);
        wait_all_reach();
        set_site(2, x_default - x_offset, y_start + y_step, z_up);
        wait_all_reach();
        return_cg(2);
        set_site(2, x_default - x_offset, y_start + y_step, z_default);
        wait_all_reach();
        move_cg(3);
        set_site(3, KEEP, KEEP , z_up);
        wait_all_reach();
        set_site(3, x_default - x_offset, y_start + y_step, z_up);
        wait_all_reach();
        return_cg(3);
        set_site(3, x_default - x_offset, y_start + y_step, z_default);
        wait_all_reach();
      }
      else
      {
        move_cg(0);
        set_site(0, KEEP, KEEP, z_up);
        wait_all_reach();
        set_site(0, x_default - x_offset, y_start + y_step, z_up);
        wait_all_reach();
        return_cg(0);
        set_site(0, x_default - x_offset, y_start + y_step, z_default);
        wait_all_reach();
        move_cg(1);
        set_site(1, KEEP, KEEP , z_up);
        wait_all_reach();
        set_site(1, x_default - x_offset, y_start + y_step, z_up);
        wait_all_reach();
        return_cg(1);
        set_site(1, x_default - x_offset, y_start + y_step, z_default);
        wait_all_reach();
      }
      break;

    case SPRAWL:
      set_site(1, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(1, x_default - x_offset, y_start + y_step, KEEP);
      wait_all_reach();

      set_site(3, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(3, x_default - x_offset, y_start + y_step, KEEP);
      wait_all_reach();

      set_site(0, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(0, x_default - x_offset, y_start + y_step, KEEP);
      wait_all_reach();

      set_site(2, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(2, x_default - x_offset, y_start + y_step, KEEP);
      wait_all_reach   ();

      for (int i = 0; i < 4; i++)
      {
        set_site(i, x_default - x_offset, y_start + y_step, z_default);
      }
      wait_all_reach();
      break;
  }
  leg_position = RELAX;
}
/*
  - JC
  - Change_legs to for a start of walk pose V on one side = on other
  - blocking function
   ---------------------------------------------------------------------------*/
void walk_legs(void)
{
  move_speed = leg_move_speed;

  switch (leg_position)
  {
    case RELAX:
      move_cg(2);
      set_site(2, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(2, x_default - x_offset, y_start, z_up);
      wait_all_reach();
      return_cg(2);
      set_site(2, x_default - x_offset, y_start, z_default);
      wait_all_reach();
      move_cg(3);
      set_site(3, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(3, x_default - x_offset, y_start, z_up);
      wait_all_reach();
      return_cg(3);
      set_site(3, x_default - x_offset, y_start, z_default);
      wait_all_reach();
      break;

    case SPRAWL:
      relax_legs();
      walk_legs();
      break;
  }
  leg_position = WALK;
}
/*
  - JC
  - Change_legs to for a start of walk pose V on one side = on other mirror image of walk_legs
  - blocking function
   ---------------------------------------------------------------------------*/
void walk_legs_other(void)
{
  move_speed = leg_move_speed;

  switch (leg_position)
  {
    case RELAX:
      move_cg(0);
      set_site(0, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(0, x_default - x_offset, y_start, z_up);
      wait_all_reach();
      return_cg(0);
      set_site(0, x_default - x_offset, y_start, z_default);
      wait_all_reach();
      move_cg(1);
      set_site(1, KEEP, KEEP, z_up);
      wait_all_reach();
      set_site(1, x_default - x_offset, y_start, z_up);
      wait_all_reach();
      return_cg(1);
      set_site(1, x_default - x_offset, y_start, z_default);
      wait_all_reach();
      break;

    case SPRAWL:
      relax_legs();
      walk_legs_other();
      break;
  }
  leg_position = WALK;
}

/*
  - JC
  - Change_legs to for a X for a sprawl pose
  - blocking function
   ---------------------------------------------------------------------------*/
void sprawl(void)
{
  move_speed = leg_move_speed;

  relax_legs();
  for (int i = 0; i < 4; i++)
  {
    set_site(i, sprawl_leg, sprawl_leg, KEEP);
  }
  wait_all_reach();
  leg_position = SPRAWL;
}
/*
  - JC
  - Change_leg_z
  - leg to change
  - blocking function
   ---------------------------------------------------------------------------*/
void change_leg_z(int leg, float z)
{
  set_site(leg, KEEP, KEEP, z);
  wait_all_reach();
}
/*
  - JC
  - move_cg
  - leg to change
  - blocking function
  - move the body of the robot away from the leg that is going to be lifted
   ---------------------------------------------------------------------------*/
void move_cg(int leg)
{
  // reducing right leg and increasing left leg X moves right
  // reducing front leg and increasing back leg Y moves forward
  float temp_move_speed = move_speed;
  move_speed = body_move_speed;

  //site_now contains current leg position - remember this
  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 3; j++)
    {
      site_cg[i][j] = site_now[i][j];
    }
    if (capture)
    {
      Serial.println("CG, " + String(i) + ", " + String(site_cg[i][0]) + ", " + String(site_cg[i][1]) + ", " + String(site_cg[i][2]));
    }
  }

  switch (leg)
  {
    case 0:  // front right leg - move body weight back and left
      set_site(0, site_cg[0][0] + cg_shift, site_cg[0][1] + cg_shift, site_cg[0][2]);
      set_site(1, site_cg[1][0] + cg_shift, site_cg[1][1] - cg_shift, site_cg[1][2]);
      set_site(2, site_cg[2][0] - cg_shift, site_cg[2][1] + cg_shift, site_cg[2][2]);
      set_site(3, site_cg[3][0] - cg_shift, site_cg[3][1] - cg_shift, site_cg[3][2]);
      break;

    case 1:  // back right leg - move body weight forward and left
      set_site(0, site_cg[0][0] + cg_shift, site_cg[0][1] - cg_shift, site_cg[0][2]);
      set_site(1, site_cg[1][0] + cg_shift, site_cg[1][1] + cg_shift, site_cg[1][2]);
      set_site(2, site_cg[2][0] - cg_shift, site_cg[2][1] - cg_shift, site_cg[2][2]);
      set_site(3, site_cg[3][0] - cg_shift, site_cg[3][1] + cg_shift, site_cg[3][2]);
      break;

    case 2:  // front left leg - move body weight back and right
      set_site(0, site_cg[0][0] - cg_shift, site_cg[0][1] + cg_shift, site_cg[0][2]);
      set_site(1, site_cg[1][0] - cg_shift, site_cg[1][1] - cg_shift, site_cg[1][2]);
      set_site(2, site_cg[2][0] + cg_shift, site_cg[2][1] + cg_shift, site_cg[2][2]);
      set_site(3, site_cg[3][0] + cg_shift, site_cg[3][1] - cg_shift, site_cg[3][2]);
      break;

    case 3:  // back left leg - move body weight forward and right
      set_site(0, site_cg[0][0] - cg_shift, site_cg[0][1] - cg_shift, site_cg[0][2]);
      set_site(1, site_cg[1][0] - cg_shift, site_cg[1][1] + cg_shift, site_cg[1][2]);
      set_site(2, site_cg[2][0] + cg_shift, site_cg[2][1] - cg_shift, site_cg[2][2]);
      set_site(3, site_cg[3][0] + cg_shift, site_cg[3][1] + cg_shift, site_cg[3][2]);
      break;
  }
  wait_all_reach();
  move_speed = temp_move_speed;

}
/*
  - JC
  - return_cg
  - leg to change
  - blocking function
  - move the body of the robot back towards lifted leg
   ---------------------------------------------------------------------------*/
void return_cg(int leg)
{
  float temp_move_speed = move_speed;
  move_speed = body_move_speed;
  for (int i = 0; i < 4; i++)
  {
    if (i != leg)
    {
      set_site(i, site_cg[i][0], site_cg[i][1], site_cg[i][2]);
    }
  }

  move_speed = temp_move_speed;

}
// -- end of JC added move routines --

/*
  - sit
  - blocking function
   ---------------------------------------------------------------------------*/
void sit(void)
{
  move_speed = stand_seat_speed;
  for (int leg = 0; leg < 4; leg++)
  {
    set_site(leg, KEEP, KEEP, z_boot);
  }
  wait_all_reach();
}

/*
  - stand
  - blocking function
   ---------------------------------------------------------------------------*/
void stand(void)
{
  move_speed = stand_seat_speed;
  for (int leg = 0; leg < 4; leg++)
  {
    set_site(leg, KEEP, KEEP, z_default);
  }
  wait_all_reach();
}


/*
  - spot turn to left
  - blocking function
  - parameter step steps wanted to turn
   ---------------------------------------------------------------------------*/
void turn_left(unsigned int step)
{
  walk_legs(); // JC - get legs in correct starting position
  move_speed = spot_turn_speed;
  while (step-- > 0)
  {
    if (site_now[3][1] == y_start)
    {
      //leg 3&1 move
      set_site(3, x_default + x_offset, y_start, z_up);
      wait_all_reach();

      set_site(0, turn_x1 - x_offset, turn_y1, z_default);
      set_site(1, turn_x0 - x_offset, turn_y0, z_default);
      set_site(2, turn_x1 + x_offset, turn_y1, z_default);
      set_site(3, turn_x0 + x_offset, turn_y0, z_up);
      wait_all_reach();

      set_site(3, turn_x0 + x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(0, turn_x1 + x_offset, turn_y1, z_default);
      set_site(1, turn_x0 + x_offset, turn_y0, z_default);
      set_site(2, turn_x1 - x_offset, turn_y1, z_default);
      set_site(3, turn_x0 - x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(1, turn_x0 + x_offset, turn_y0, z_up);
      wait_all_reach();

      set_site(0, x_default + x_offset, y_start, z_default);
      set_site(1, x_default + x_offset, y_start, z_up);
      set_site(2, x_default - x_offset, y_start + y_step, z_default);
      set_site(3, x_default - x_offset, y_start + y_step, z_default);
      wait_all_reach();

      set_site(1, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
    else
    {
      //leg 0&2 move
      set_site(0, x_default + x_offset, y_start, z_up);
      wait_all_reach();

      set_site(0, turn_x0 + x_offset, turn_y0, z_up);
      set_site(1, turn_x1 + x_offset, turn_y1, z_default);
      set_site(2, turn_x0 - x_offset, turn_y0, z_default);
      set_site(3, turn_x1 - x_offset, turn_y1, z_default);
      wait_all_reach();

      set_site(0, turn_x0 + x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(0, turn_x0 - x_offset, turn_y0, z_default);
      set_site(1, turn_x1 - x_offset, turn_y1, z_default);
      set_site(2, turn_x0 + x_offset, turn_y0, z_default);
      set_site(3, turn_x1 + x_offset, turn_y1, z_default);
      wait_all_reach();

      set_site(2, turn_x0 + x_offset, turn_y0, z_up);
      wait_all_reach();

      set_site(0, x_default - x_offset, y_start + y_step, z_default);
      set_site(1, x_default - x_offset, y_start + y_step, z_default);
      set_site(2, x_default + x_offset, y_start, z_up);
      set_site(3, x_default + x_offset, y_start, z_default);
      wait_all_reach();

      set_site(2, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
  }
}

/*
  - spot turn to right
  - blocking function
  - parameter step steps wanted to turn
   ---------------------------------------------------------------------------*/
void turn_right(unsigned int step)
{
  walk_legs(); // JC - get legs in correct starting position
  move_speed = spot_turn_speed;
  while (step-- > 0)
  {
    if (site_now[2][1] == y_start)
    {
      //leg 2&0 move
      set_site(2, x_default + x_offset, y_start, z_up);
      wait_all_reach();

      set_site(0, turn_x0 - x_offset, turn_y0, z_default);
      set_site(1, turn_x1 - x_offset, turn_y1, z_default);
      set_site(2, turn_x0 + x_offset, turn_y0, z_up);
      set_site(3, turn_x1 + x_offset, turn_y1, z_default);
      wait_all_reach();

      set_site(2, turn_x0 + x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(0, turn_x0 + x_offset, turn_y0, z_default);
      set_site(1, turn_x1 + x_offset, turn_y1, z_default);
      set_site(2, turn_x0 - x_offset, turn_y0, z_default);
      set_site(3, turn_x1 - x_offset, turn_y1, z_default);
      wait_all_reach();

      set_site(0, turn_x0 + x_offset, turn_y0, z_up);
      wait_all_reach();

      set_site(0, x_default + x_offset, y_start, z_up);
      set_site(1, x_default + x_offset, y_start, z_default);
      set_site(2, x_default - x_offset, y_start + y_step, z_default);
      set_site(3, x_default - x_offset, y_start + y_step, z_default);
      wait_all_reach();

      set_site(0, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
    else
    {
      //leg 1&3 move
      set_site(1, x_default + x_offset, y_start, z_up);
      wait_all_reach();

      set_site(0, turn_x1 + x_offset, turn_y1, z_default);
      set_site(1, turn_x0 + x_offset, turn_y0, z_up);
      set_site(2, turn_x1 - x_offset, turn_y1, z_default);
      set_site(3, turn_x0 - x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(1, turn_x0 + x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(0, turn_x1 - x_offset, turn_y1, z_default);
      set_site(1, turn_x0 - x_offset, turn_y0, z_default);
      set_site(2, turn_x1 + x_offset, turn_y1, z_default);
      set_site(3, turn_x0 + x_offset, turn_y0, z_default);
      wait_all_reach();

      set_site(3, turn_x0 + x_offset, turn_y0, z_up);
      wait_all_reach();

      set_site(0, x_default - x_offset, y_start + y_step, z_default);
      set_site(1, x_default - x_offset, y_start + y_step, z_default);
      set_site(2, x_default + x_offset, y_start, z_default);
      set_site(3, x_default + x_offset, y_start, z_up);
      wait_all_reach();

      set_site(3, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
  }
}

/*
  - go forward
  - blocking function
  - parameter step steps wanted to go
   ---------------------------------------------------------------------------*/
void step_forward(unsigned int step)
{
  walk_legs(); // JC - get legs in correct starting position
  move_speed = leg_move_speed;
  while (step-- > 0)
  {
    if (site_now[2][1] == y_start)
    {
      //leg 2&1 move
      set_site(2, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(2, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(2, x_default + x_offset, y_start + 2 * y_step, z_default);
      wait_all_reach();

      move_speed = body_move_speed;

      set_site(0, x_default + x_offset, y_start, z_default);
      set_site(1, x_default + x_offset, y_start + 2 * y_step, z_default);
      set_site(2, x_default - x_offset, y_start + y_step, z_default);
      set_site(3, x_default - x_offset, y_start + y_step, z_default);
      wait_all_reach();

      move_speed = leg_move_speed;

      set_site(1, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(1, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(1, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
    else
    {
      //leg 0&3 move
      set_site(0, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(0, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(0, x_default + x_offset, y_start + 2 * y_step, z_default);
      wait_all_reach();

      move_speed = body_move_speed;

      set_site(0, x_default - x_offset, y_start + y_step, z_default);
      set_site(1, x_default - x_offset, y_start + y_step, z_default);
      set_site(2, x_default + x_offset, y_start, z_default);
      set_site(3, x_default + x_offset, y_start + 2 * y_step, z_default);
      wait_all_reach();

      move_speed = leg_move_speed;

      set_site(3, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(3, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(3, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
  }
}

/*
  - go back
  - blocking function
  - parameter step steps wanted to go
   ---------------------------------------------------------------------------*/
void step_back(unsigned int step)
{
  walk_legs(); // JC - get legs in correct starting position
  move_speed = leg_move_speed;
  while (step-- > 0)
  {
    if (site_now[3][1] == y_start)
    {
      //leg 3&0 move
      set_site(3, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(3, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(3, x_default + x_offset, y_start + 2 * y_step, z_default);
      wait_all_reach();

      move_speed = body_move_speed;

      set_site(0, x_default + x_offset, y_start + 2 * y_step, z_default);
      set_site(1, x_default + x_offset, y_start, z_default);
      set_site(2, x_default - x_offset, y_start + y_step, z_default);
      set_site(3, x_default - x_offset, y_start + y_step, z_default);
      wait_all_reach();

      move_speed = leg_move_speed;

      set_site(0, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(0, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(0, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
    else
    {
      //leg 1&2 move
      set_site(1, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(1, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(1, x_default + x_offset, y_start + 2 * y_step, z_default);
      wait_all_reach();

      move_speed = body_move_speed;

      set_site(0, x_default - x_offset, y_start + y_step, z_default);
      set_site(1, x_default - x_offset, y_start + y_step, z_default);
      set_site(2, x_default + x_offset, y_start + 2 * y_step, z_default);
      set_site(3, x_default + x_offset, y_start, z_default);
      wait_all_reach();

      move_speed = leg_move_speed;

      set_site(2, x_default + x_offset, y_start + 2 * y_step, z_up);
      wait_all_reach();
      set_site(2, x_default + x_offset, y_start, z_up);
      wait_all_reach();
      set_site(2, x_default + x_offset, y_start, z_default);
      wait_all_reach();
    }
  }
}

// add by RegisHsu

void body_left(int i)
{
  set_site(0, site_now[0][0] + i, KEEP, KEEP);
  set_site(1, site_now[1][0] + i, KEEP, KEEP);
  set_site(2, site_now[2][0] - i, KEEP, KEEP);
  set_site(3, site_now[3][0] - i, KEEP, KEEP);
  wait_all_reach();
}

void body_right(int i)
{
  set_site(0, site_now[0][0] - i, KEEP, KEEP);
  set_site(1, site_now[1][0] - i, KEEP, KEEP);
  set_site(2, site_now[2][0] + i, KEEP, KEEP);
  set_site(3, site_now[3][0] + i, KEEP, KEEP);
  wait_all_reach();
}

void hand_wave(int i)
{
  float x_tmp;
  float y_tmp;
  float z_tmp;
  walk_legs(); // JC - get legs in correct starting position
  move_speed = 1;
  if (site_now[3][1] == y_start)
  {
    body_right(15);
    x_tmp = site_now[2][0];
    y_tmp = site_now[2][1];
    z_tmp = site_now[2][2];
    move_speed = body_move_speed;
    for (int j = 0; j < i; j++)
    {
      set_site(2, turn_x1, turn_y1, 50);
      wait_all_reach();
      set_site(2, turn_x0, turn_y0, 50);
      wait_all_reach();
    }
    set_site(2, x_tmp, y_tmp, z_tmp);
    wait_all_reach();
    move_speed = 1;
    body_left(15);
  }
  else
  {
    body_left(15);
    x_tmp = site_now[0][0];
    y_tmp = site_now[0][1];
    z_tmp = site_now[0][2];
    move_speed = body_move_speed;
    for (int j = 0; j < i; j++)
    {
      set_site(0, turn_x1, turn_y1, 50);
      wait_all_reach();
      set_site(0, turn_x0, turn_y0, 50);
      wait_all_reach();
    }
    set_site(0, x_tmp, y_tmp, z_tmp);
    wait_all_reach();
    move_speed = 1;
    body_right(15);
  }
}

void hand_shake(int i)
{
  float x_tmp;
  float y_tmp;
  float z_tmp;
  walk_legs(); // JC - get legs in correct starting position
  move_speed = 1;
  if (site_now[3][1] == y_start)
  {
    body_right(15);
    x_tmp = site_now[2][0];
    y_tmp = site_now[2][1];
    z_tmp = site_now[2][2];
    move_speed = body_move_speed;
    for (int j = 0; j < i; j++)
    {
      set_site(2, x_default - 30, y_start + 2 * y_step, 55);
      wait_all_reach();
      set_site(2, x_default - 30, y_start + 2 * y_step, 10);
      wait_all_reach();
    }
    set_site(2, x_tmp, y_tmp, z_tmp);
    wait_all_reach();
    move_speed = 1;
    body_left(15);
  }
  else
  {
    body_left(15);
    x_tmp = site_now[0][0];
    y_tmp = site_now[0][1];
    z_tmp = site_now[0][2];
    move_speed = body_move_speed;
    for (int j = 0; j < i; j++)
    {
      set_site(0, x_default - 30, y_start + 2 * y_step, 55);
      wait_all_reach();
      set_site(0, x_default - 30, y_start + 2 * y_step, 10);
      wait_all_reach();
    }
    set_site(0, x_tmp, y_tmp, z_tmp);
    wait_all_reach();
    move_speed = 1;
    body_right(15);
  }
}




/*
  - set one of end points' expect site
  - this founction will set temp_speed[4][3] at same time
  - non - blocking function
   ---------------------------------------------------------------------------*/
void set_site(int leg, float x, float y, float z)
{
  float length_x = 0, length_y = 0, length_z = 0;

  if (capture)
  {
    Serial.println("P, " + String(leg) + ", " + String(x) + ", " + String(y) + ", " + String(z)); // log the set leg position
  }

  if (x != KEEP)
    length_x = x - site_now[leg][0];
  if (y != KEEP)
    length_y = y - site_now[leg][1];
  if (z != KEEP)
    length_z = z - site_now[leg][2];

  float length = sqrt(pow(length_x, 2) + pow(length_y, 2) + pow(length_z, 2));

  temp_speed[leg][0] = length_x / length * move_speed * speed_multiple;
  temp_speed[leg][1] = length_y / length * move_speed * speed_multiple;
  temp_speed[leg][2] = length_z / length * move_speed * speed_multiple;

  if (x != KEEP)
    site_expect[leg][0] = x;
  if (y != KEEP)
    site_expect[leg][1] = y;
  if (z != KEEP)
    site_expect[leg][2] = z;
}

/*
  - wait one of end points move to expect site
  - blocking function
   ---------------------------------------------------------------------------*/
void wait_reach(int leg)
{
  boolean rc = false;
  while (1) {
    rc = check_ir();
    if (site_now[leg][0] == site_expect[leg][0])
      if (site_now[leg][1] == site_expect[leg][1])
        if (site_now[leg][2] == site_expect[leg][2])
          break;
  }
  if (rc)longjmp(jump_env, 2);
}

/*
  - wait all of end points move to expect site
  - blocking function
   ---------------------------------------------------------------------------*/
void wait_all_reach(void)
{
  for (int i = 0; i < 4; i++)
    wait_reach(i);
}


/*
  - microservos service /timer interrupt function/50Hz
  - when set site expected,this function move the end point to it in a straight line
  - temp_speed[4][3] should be set before set expect site,it make sure the end point
   move in a straight line,and decide move speed.
   ---------------------------------------------------------------------------*/
SIGNAL(TIMER0_COMPA_vect) {
  static float alpha, beta, gamma;
  sei();
  static long old_time = cur_time;
  if (cur_time - old_time >= 20) {
    old_time = cur_time;
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 3; j++)
      {
        if (abs(site_now[i][j] - site_expect[i][j]) >= abs(temp_speed[i][j]))
          site_now[i][j] += temp_speed[i][j];
        else
          site_now[i][j] = site_expect[i][j];
      }
      cartesian_to_polar(alpha, beta, gamma, site_now[i][0], site_now[i][1], site_now[i][2]);
      polar_to_servo(i, alpha, beta, gamma);
    }
  }
  cur_time = millis();
}
/*
  - trans site from cartesian to polar
  - mathematical model 2/2
   ---------------------------------------------------------------------------*/
void cartesian_to_polar(volatile float &alpha, volatile float &beta, volatile float &gamma, volatile float x, volatile float y, volatile float z)
{
  //calculate w-z degree
  float v, w;
  w = (x >= 0 ? 1 : -1) * (sqrt(pow(x, 2) + pow(y, 2)));
  v = w - coxa;

  alpha = atan2(z, v) + acos((pow(femur, 2) - pow(tibia, 2) + pow(v, 2) + pow(z, 2)) / 2 / femur / sqrt(pow(v, 2) + pow(z, 2)));

  beta = acos((pow(femur, 2) + pow(tibia, 2) - pow(v, 2) - pow(z, 2)) / 2 / femur / tibia);

  //calculate x-y-z degree
  gamma = (w >= 0) ? atan2(y, x) : atan2(-y, -x);
  //trans degree pi->180
  alpha = alpha / pi * 180;
  beta = beta / pi * 180;
  gamma = gamma / pi * 180;
}

/*
  - trans site from polar to microservos
  - mathematical model map to fact
  - the errors saved in eeprom will be add
   ---------------------------------------------------------------------------*/
void polar_to_servo(int leg, float alpha, float beta, float gamma)
{
  alpha += servo_error[leg][0];
  beta += servo_error[leg][1];
  gamma += servo_error[leg][2];

  if (leg == 0)
  {
    alpha = 90 - alpha;
    beta = beta;
    gamma += 90;
  }
  else if (leg == 1)
  {
    alpha += 90;
    beta = 180 - beta;
    gamma = 90 - gamma;
  }
  else if (leg == 2)
  {
    alpha += 90;
    beta = 180 - beta;
    gamma = 90 - gamma;
  }
  else if (leg == 3)
  {
    alpha = 90 - alpha;
    beta = beta;
    gamma += 90;
  }

  if (capture)  // debug data
  {
    //    Serial.println("A, "+String(leg)+","+String(alpha)+","+String(beta)+","+String(gamma));
  }

  servo[leg][femur_servo_index].write(alpha + servo_error[leg][femur_servo_index]);
  servo[leg][tibia_servo_index].write(beta + servo_error[leg][tibia_servo_index]);
  servo[leg][coxa_servo_index].write(gamma + servo_error[leg][coxa_servo_index]);

}

void delay(int period) {
  long timeout = millis() + period;
  while (millis() <= timeout) {
    if (check_ir())
      longjmp(jump_env, 1);
  }
}

void storeTrim() {
  int addr = EEPROM_OFFSET;
  String s("");

  for (int i = 0; i < 4; i++) {
    s = s + "{";
    for (int j = 0; j < 3; j++) {
      EEPROMWriteWord(addr, servo_error[i][j]);
      s = s + String(servo_error[i][j]) + " ";
      delay(100);
      addr += 2;
    }
    s = s + "}";
  }
  Serial.println("Saved: " + s);
}

void loadTrim() {
  int addr = EEPROM_OFFSET;
  String s("");

  for (int i = 0; i < 4; i++) {
    s = s + "{";
    for (int j = 0; j < 3; j++) {
      servo_error[i][j] = EEPROMReadWord(addr);
      s = s + String(servo_error[i][j]) + " ";
      //Serial.println(servo_error[i][j]);
      addr += 2;
    }
    s = s + "}";
  }
  Serial.println("Stored: " + s);
}

int EEPROMReadWord(int p_address)
{
  byte lowByte = EEPROM.read(p_address);
  byte highByte = EEPROM.read(p_address + 1);

  return ((lowByte << 0) & 0xFF) + ((highByte << 8) & 0xFF00);
}

void EEPROMWriteWord(int p_address, int p_value)
{
  byte lowByte = ((p_value >> 0) & 0xFF);
  byte highByte = ((p_value >> 8) & 0xFF);

  EEPROM.write(p_address, lowByte);
  EEPROM.write(p_address + 1, highByte);
}
