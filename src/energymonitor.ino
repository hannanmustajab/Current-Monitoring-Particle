/*
 * Project : energymonitor
 * Description:
 * Author: Abdul Hannan Mustajab
 * Date: 20 July 2020
 */

#define SOFTWARERELEASENUMBER "1.01"                                                        // Keep track of release numbers

// Prototypes and System Mode calls

SYSTEM_MODE(SEMI_AUTOMATIC);                                                               // This will enable user code to start executing automatically.
SYSTEM_THREAD(ENABLED);                                                                    // Means my code will not be held up by Particle processes.
STARTUP(System.enableFeature(FEATURE_RESET_INFO));

// State Machine Variables

enum State { INITIALIZATION_STATE, ERROR_STATE, IDLE_STATE, MEASURING_STATE, REPORTING_STATE, RESP_WAIT_STATE};
char stateNames[8][26] = {"Initialize", "Error", "Idle", "Measuring","Reporting", "Response Wait"};
State state = INITIALIZATION_STATE;
State oldState = INITIALIZATION_STATE;

// Include Libraries

#include "EmonLib.h"                   // Include Emon Library
EnergyMonitor emon1;                   // Create an instance

// Pin Constants
const int currentSensorPin = 2;

// Timing Variables
int publishInterval;                                                                        // Publish interval for sending data. 
const unsigned long webhookWait = 45000;                                                    // How long will we wair for a WebHook response
const unsigned long resetWait   = 300000;                                                   // How long will we wait in ERROR_STATE until reset
unsigned long webhookTimeStamp  = 0;                                                        // Webhooks...
int sampleRate;                                                                             // Sample rate for idle state.
time_t t;    
const int publishFrequency      = 1000;                                                     // We can only publish once a second
unsigned long resetTimeStamp    = 0;                                                        // Resets - this keeps you from falling into a reset loop
unsigned long lastPublish       = 0;
byte currentHourlyPeriod;                                                                   // This is where we will know if the period changed
time_t currentCountTime;                                                                    // Global time vairable
byte currentMinutePeriod;                                                                   // control timing when using 5-min samp intervals

// Variables
double current_irms             = 0;
double previous_irms            = 0;
double raw_irms                 = 0;
//Program control variables
int resetCount;                                                                             // Counts the number of times the Electron has had a pin reset
int alertCount;                                                                             // Keeps track of non-reset issues - think of it as an indication of health
bool dataInFlight = true;
const char* releaseNumber = SOFTWARERELEASENUMBER;                                          // Displays the release on the menu
byte controlRegister;                                                                       // Stores the control register values
bool verboseMode=0;     

// setup() runs once, when the device is first turned on.
void setup() {
  Serial.begin(9600);

  Particle.function("verboseMode",setVerboseMode);                                          // Added Particle Function For VerboseMode.
  Particle.function("Get-Reading", senseNow);                                               // This function will force it to get a reading and set the refresh rate to 15mins.
  Particle.function("Send-Report", sendNow);                                                // This function will force it to get a reading and set the refresh rate to 15mins.
  
  char StartupMessage[64] = "Startup Successful";                                                       // Messages from Initialization
  state = IDLE_STATE;

  char responseTopic[125];
  String deviceID = System.deviceID();                                                      // Multiple Electrons share the same hook - keeps things straight
  deviceID.toCharArray(responseTopic,125);
  Particle.subscribe(responseTopic, UbidotsHandler, MY_DEVICES);                            // Subscribe to the integration response event
  
  if(!connectToParticle()) {
    state = ERROR_STATE;                                                                               // We failed to connect can reset here or go to the ERROR state for remediation
    resetTimeStamp = millis();
    snprintf(StartupMessage, sizeof(StartupMessage), "Failed to connect");
  }

  emon1.current(A2, 85);                                                                 // Current: input pin, calibration.
  takeMeasurements();
  
  if(verboseMode) Particle.publish("Startup",StartupMessage,PRIVATE);                                 // Let Particle know how the startup process went
  
  lastPublish = millis();
}

void loop() {
    
  switch(state) {
  
  case IDLE_STATE:                                                                          // Stay here if the device is turned off. 
    {
      
      if (verboseMode && oldState != state) publishStateTransition();                    // If verboseMode is on and state is changed, Then publish the state transition.

      static int TimePassed = 0;
  
      if ((Time.second() - TimePassed >= 5) || Time.minute() != currentMinutePeriod) {     // Sample time or the top of the hour
          state = MEASURING_STATE;
          TimePassed = Time.second();
      }
      break;
      
    } 

  case MEASURING_STATE:                                                                     // Take measurements prior to sending
    if (verboseMode && state != oldState) publishStateTransition();

    if (!takeMeasurements())
    {
      state = IDLE_STATE;
    }
   
    else {
      state = REPORTING_STATE;
      previous_irms = current_irms;
    }
    break;

  case REPORTING_STATE:
    if (verboseMode && state != oldState) publishStateTransition();                         // Reporting - hourly or on command
   
    if (Particle.connected()) {
      if (Time.hour() == 12) Particle.syncTime();                                           // Set the clock each day at noon
      sendEvent();                                                                          // Send data to Ubidots
      state = RESP_WAIT_STATE;                                                              // Wait for Response
    }
    else {
      state = ERROR_STATE;
      resetTimeStamp = millis();
    }
    break;

  case RESP_WAIT_STATE:
    if (verboseMode && state != oldState) publishStateTransition();
    if (!dataInFlight)                                                // Response received back to IDLE state
    {
     state = IDLE_STATE;
    }
    else if (millis() - webhookTimeStamp > webhookWait) {             // If it takes too long - will need to reset
      resetTimeStamp = millis();
      Particle.publish("spark/device/session/end", "", PRIVATE);      // If the device times out on the Webhook response, it will ensure a new session is started on next connect
      state = ERROR_STATE;                                            // Response timed out
      resetTimeStamp = millis();
    }
    break;

  
  case ERROR_STATE:                                                                         // To be enhanced - where we deal with errors
    if (verboseMode && state != oldState) publishStateTransition();
    if (millis() > resetTimeStamp + resetWait)
    {
      if (Particle.connected()) Particle.publish("State","Error State - Reset", PRIVATE);    // Brodcast Reset Action
      delay(2000);
      System.reset();
    }
    break;
  }
}



void sendEvent()
{
  char data[64];
  snprintf(data, sizeof(data), "{\"current\":%3.1f, \"raw_current\":%f}",current_irms,raw_irms);
  Particle.publish("current-webhook", data, PRIVATE);
  currentMinutePeriod = Time.minute();                                                        // Change the time period
  dataInFlight = true;                                                                      // set the data inflight flag
  webhookTimeStamp = millis();
}

void UbidotsHandler(const char *event, const char *data)                                    // Looks at the response from Ubidots - Will reset Photon if no successful response
{                                                                                           // Response Template: "{{hourly.0.status_code}}" so, I should only get a 3 digit number back
    // Response Template: "{{hourly.0.status_code}}"
  if (!data) {                                                                    // First check to see if there is any data
    if (verboseMode) {
      waitUntil(meterParticlePublish);
      Particle.publish("Ubidots Hook", "No Data", PRIVATE);
    }
    return;
  }
  int responseCode = atoi(data);                                                  // Response is only a single number thanks to Template
  
  if ((responseCode == 200) || (responseCode == 201))
  {
    if (verboseMode) {
      waitUntil(meterParticlePublish);
      Particle.publish("State", "Response Received", PRIVATE);
    }
    dataInFlight = false;    
  }
  else if (verboseMode) {
    waitUntil(meterParticlePublish);      
    Particle.publish("Ubidots Hook", data, PRIVATE);                              // Publish the response code
  }

}

bool takeMeasurements(){
  current_irms = emon1.calcIrms(1480);                                               // Calculate Irms only
  raw_irms = analogRead(A2);
  // waitUntil(meterParticlePublish);
  // Particle.publish("Irms",String(current_irms),PRIVATE);
  // Particle.publish("Sensor",String((current_irms - previous_irms)),PRIVATE);
  // Particle.publish("lastIrms",String(abs(previous_irms)),PRIVATE);
  if (abs(current_irms - previous_irms) > 0.5){
    raw_irms = analogRead(A2);
    Particle.publish("RAW",String(raw_irms),PRIVATE);
    return 1;
  } 
}


// These are the particle functions that allow you to configure and run the device
// They are intended to allow for customization and control during installations
// and to allow for management.


// These functions control the connection and disconnection from Particle
bool connectToParticle() {
  Particle.connect();
  // wait for *up to* 5 minutes
  for (int retry = 0; retry < 300 && !waitFor(Particle.connected,1000); retry++) {
    Particle.process();
  }
  if (Particle.connected()) return 1;                               // Were able to connect successfully
  else return 0;                                                    // Failed to connect
}

int setVerboseMode(String command) // Function to force sending data in current hour
{
  if (command == "1")
  {
    verboseMode = true;
    Particle.publish("Mode","Set Verbose Mode",PRIVATE);
    return 1;
  }
  else if (command == "0")
  {
    verboseMode = false;
    Particle.publish("Mode","Cleared Verbose Mode",PRIVATE);
    return 1;
  }
  else return 0;
}


void publishStateTransition(void)
{
  char stateTransitionString[40];
  snprintf(stateTransitionString, sizeof(stateTransitionString), "From %s to %s", stateNames[oldState],stateNames[state]);
  oldState = state;
  if(Particle.connected()) {
    waitUntil(meterParticlePublish);
    Particle.publish("State Transition",stateTransitionString, PRIVATE);
    lastPublish = millis();
  }
  Serial.println(stateTransitionString);
}

bool meterParticlePublish(void)
{
  if(millis() - lastPublish >= publishFrequency) return 1;
  else return 0;
}

bool senseNow(String Command)                                                      // This command lets you force a reporting cycle
{
  if (Command == "1") {
    state = MEASURING_STATE;                                                      // Set the state to reporting
    waitUntil(meterParticlePublish);  
    Particle.publish("Function", "Command accepted - sensing now",PRIVATE);       // Acknowledge receipt
    return 1;
  }
  else if (Command == "0") {                                                      // No action required
    return 1;

  }
  return 0;
}

bool sendNow(String Command)                                                      // This command lets you force a reporting cycle
{
  if (Command == "1") {
    state = REPORTING_STATE;                                                      // Set the state to reporting
    waitUntil(meterParticlePublish);  
    Particle.publish("Function", "Command accepted - reporting now",PRIVATE);     // Acknowledge receipt
    return 1;
  }
  else if (Command == "0") {                                                      // No action required
    return 1;
  }
  return 0;
}
