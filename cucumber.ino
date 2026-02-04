/*
	"cucumber" watering system code
	© 2026 Ashley Kollmorgen. All rights reserved.
	
	External libraries used:
	- "Adafruit BusIO" by Adafruit
	- "Adafruit GFX Library" by Adafruit
	- "Adafruit SSD1306" by Adafruit
	- "DHTlib" by Rob Tillaart
	- "DS3231-RTC" by Frank Häfele
*/



#include <DS3231-RTC.h>
#include <dht.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>



// user configuration

const struct {
	const struct {
		float tempCoeff = -9;
		float rainCoeff = 27;
		float maxTally = 180;
		float maxRain = 20;

		const int desiredLevels[6] = { 20, 20, 20, 20, 20, 20 };
		const int bufferSize[6] = { 60, 60, 60, 60, 60, 60 };
	} watering;

	const struct {
		const int fertTimes[6] = { 20, 20, 20, 20, 20, 20 };  // seconds for which to fertilise each line per cycle (3 cycles total per fertilisation)
	} fertilising;

	const struct {
		int joystickSensitivity = 300;
		int buttonSensitivity = 100;
	} menuJoystick;

	bool resetPersistentMemory = false;  // resets the persistent memory on startup when true
} settings;



// type to hold historical data for each day
typedef struct {
	int day = -1;
	float maxTemp = 0;
	int rainFlips = 0;
	unsigned long waterTankLevel = 0;
	unsigned int totalAdded = 0;
} Day;



const struct {
	const struct {
		int width = 128;
		int height = 64;
		int resetPin = -1;
		uint8_t textSize = 2;
	} oled;
} hardwareConfig;



const struct {
	const struct {
		int modeCount = 9;
		int mainScreenLevels = 4;
	} menu;

	const struct {
		int automationSwitches = 4;
	} persistentMemory;

	const char daysOfTheWeek[7][4] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

	int waterTankHeight = 100;
} softwareConstants;



// pin configuration

// NOTE: for the Mega, pins 2, 3, 18, 19, 20, 21 are interrupt pins except for 20 and 21 while I2C is being used, which it probably is in this case due to the RTC and/or the oled display
// NOTE: don't forget to initialise these using pinMode() as well
const struct {
	const struct {
		int trigger = 43;
		int echo = 3;
	} waterTankUltrasonic;

	const struct {
		const int spin[6] = { 7, 5, 4, 39, 25, 27 };
		const int read[6] = { 8, 6, 26, 23, 28, 31 };
	} haxSolenoid;

	const struct {
		int pump = 52;
		int fertiliser = 50;
		int tapWater = 47;
	} fertiliser;

	int tempSensor = 9;

	int rainSensor = 18;

	const struct {
		int x = A0;
		int y = A1;
		int button = A2;
	} menuJoystick;
} pins;



// global variables
struct {
	struct {
		bool desiredState[6] = { 0 };
		int oldState[6] = { 0 };
		int lock = -1;
	} haxSolenoidOn;

	struct {
		int haxTimerState[6] = { 0 };
		time_t triggers[6] = { 0 };
		bool isOn[6] = { 0 };
	} haxTimers;

	volatile bool rainSensorTripped = false;

	int lastSecond = -1;

	volatile unsigned long waterTankUltrasonicTimer = 0;

	struct {
		int currentSolenoid = -2;

		float tallies[6] = { 0 };

		time_t finishTime = 0;
	} watering;

	struct {
		time_t startTime = 0;

		int currentSolenoid = -2;
		int dumpCount = 0;

		time_t finishTime = 0;
	} fertilising;

	bool firstLoop = true;

	struct {
		int mode = 0;
		int level = 0;
		bool activate = false;
		bool changed = false;
	} menu;

	struct {
		int8_t x = 0;
		int8_t y = 0;
		bool button = false;
	} menuJoystick;

	int noPinInterrupts = 0;

	Day previousDays[3];

	DateTime time;

	struct {
		bool fertilise = true;
		bool water = true;
		bool fertPump = true;
	} automation;
} state;



// module class instance declarations
const struct {
	Adafruit_SSD1306 oled = Adafruit_SSD1306(hardwareConfig.oled.width, hardwareConfig.oled.height, &Wire, hardwareConfig.oled.resetPin);

	dht tempSensor;
} modules;



void setup() {
	// open serial port
	Serial.begin(57600);
	while (!Serial) {
		;
	}

	// start i2c
	Wire.begin();

	// try to initialise the oled display
	if (!modules.oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
		Serial.println("The display is stuffed at the moment.");
	}

	// format the text on the display
	modules.oled.setTextSize(hardwareConfig.oled.textSize);
	modules.oled.setTextColor(WHITE, BLACK);

	// enable the correct Code Page 437 character sequence
	modules.oled.cp437(true);

	// clear the display and display the cleared screen
	modules.oled.clearDisplay();
	modules.oled.display();



	// update EEPROM and persistent variables

	if (settings.resetPersistentMemory) {
		updatePersistentMemory();
	}

	state.automation.fertilise = getEEPROMBit(softwareConstants.persistentMemory.automationSwitches, 0);
	state.automation.water = getEEPROMBit(softwareConstants.persistentMemory.automationSwitches, 1);
	state.automation.fertPump = getEEPROMBit(softwareConstants.persistentMemory.automationSwitches, 2);



	// set all the pin modes

	pinMode(pins.waterTankUltrasonic.trigger, OUTPUT);
	pinMode(pins.waterTankUltrasonic.echo, INPUT);

	// set hacked solenoid pin modes
	for (int i = 0; i < sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read); i++) {
		pinMode(pins.haxSolenoid.spin[i], OUTPUT);
		pinMode(pins.haxSolenoid.read[i], INPUT);

		// set the initial water-point tallies to their desired levels initially so we don't deluge the garden at startup
		state.watering.tallies[i] = settings.watering.desiredLevels[i];

		// spin the solenoids for a few seconds so that we make sure there's a state change so that they end up in the exact right position
		state.haxSolenoidOn.oldState[i] = -1;
	}

	pinMode(pins.fertiliser.pump, OUTPUT);
	pinMode(pins.fertiliser.fertiliser, OUTPUT);
	pinMode(pins.fertiliser.tapWater, OUTPUT);

	pinMode(pins.tempSensor, INPUT);

	pinMode(pins.rainSensor, INPUT);

	pinMode(pins.menuJoystick.x, INPUT);
	pinMode(pins.menuJoystick.y, INPUT);
	pinMode(pins.menuJoystick.button, INPUT);



	// make the first state.time value equal to the current actual time
	state.time = RTClib::now();

	// make the current day in history a valid day
	state.previousDays[2].day = state.time.getWeekDay();



	// attach the interrupts for the ultrasonic sensor
	attachInterrupt(digitalPinToInterrupt(pins.waterTankUltrasonic.echo), waterTankUltrasonicEcho, CHANGE);
}



void loop() {
	state.time = RTClib::now();

	// spin the solenoids until they reach their intended positions
	for (int i = 0; i < sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read); i++) {
		const bool newState = digitalRead(pins.haxSolenoid.read[i]);

		if (state.haxSolenoidOn.oldState[i] < 0 || newState != state.haxSolenoidOn.desiredState[i]) {
			if (state.haxSolenoidOn.lock == i || state.haxSolenoidOn.lock < 0) {
				if (state.haxSolenoidOn.oldState[i] < 0 && state.time.getSecond() != state.lastSecond) {
					state.haxSolenoidOn.oldState[i]++;
				}
				digitalWrite(pins.haxSolenoid.spin[i], HIGH);
				state.haxSolenoidOn.lock = i;
			}
		} else if (state.haxSolenoidOn.oldState[i] != newState) {
			digitalWrite(pins.haxSolenoid.spin[i], LOW);
			state.haxSolenoidOn.lock = -1;
		}

		if (state.haxSolenoidOn.oldState[i] >= 0) {
			state.haxSolenoidOn.oldState[i] = newState;
		}
	}

	// increment the number of rain flips on the current day in history when the rain sensor flips
	if (digitalRead(pins.rainSensor) && !state.rainSensorTripped) {
		state.rainSensorTripped = true;
		state.previousDays[2].rainFlips++;
	}

	// check the timed procedures every second
	if (state.time.getSecond() != state.lastSecond) {
		// count down state.noPinInterrupts so it re-enables pin interrupts after two seconds at most
		if (state.noPinInterrupts) {
			state.noPinInterrupts--;
		}

		// reset for the next rain flip
		if (!digitalRead(pins.rainSensor)) {
			state.rainSensorTripped = false;
		}

		// do certain time consuming actions every minute
		if (timeIs(-1, -1, -1, 0) || state.firstLoop) {
			// read the temperature sensor and keep track of the maximum temperature
			modules.tempSensor.read11(pins.tempSensor);
			if (modules.tempSensor.temperature > state.previousDays[2].maxTemp) {
				state.previousDays[2].maxTemp = modules.tempSensor.temperature;
			}

			if (!state.noPinInterrupts) {
				// kick off the water tank ultrasonic sensor reading
				digitalWrite(pins.waterTankUltrasonic.trigger, LOW);
				delayMicroseconds(2);
				digitalWrite(pins.waterTankUltrasonic.trigger, HIGH);
				delayMicroseconds(10);
				digitalWrite(pins.waterTankUltrasonic.trigger, LOW);
			}

			// write data to screen
			if (state.menu.mode == 0) {
				state.menu.changed = true;
			}
		}

		// turn the pump on or off at certain times
		if (timeIs(-1, 7, 10, 0) && state.automation.fertPump) {
			safeRelayWrite(pins.fertiliser.pump, HIGH);
		}
		if (timeIs(-1, 7, 12, 0) && state.automation.fertPump) {
			safeRelayWrite(pins.fertiliser.pump, LOW);
		}

		// kick off the solenoid watering sequence
		if (timeIs(-1, 8, 0, 0)) {
			state.previousDays[0] = state.previousDays[1];
			state.previousDays[1] = state.previousDays[2];

			state.previousDays[2].day = state.time.getWeekDay();
			state.previousDays[2].totalAdded = 0;
			state.previousDays[2].maxTemp = modules.tempSensor.temperature;
			state.previousDays[2].rainFlips = 0;

			if (state.automation.water) {
				state.watering.currentSolenoid = -1;
			}
		}

		// iterate through the solenoids, calculate how much water they need to dispense and turn them on for an appropriate length of time
		if (state.watering.currentSolenoid >= -1 && state.watering.finishTime <= state.time.getUnixTime()) {
			if (state.watering.currentSolenoid >= 0) {
				haxSolenoidSet(state.watering.currentSolenoid, false);
			}

			state.watering.currentSolenoid++;

			if (state.watering.currentSolenoid >= sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read)) {
				state.watering.currentSolenoid = -2;
				state.watering.finishTime = 0;
			} else {
				unsigned int added = calcWater(1, state.watering.currentSolenoid);  // calculate the water to be added from day 1's data and for the current line

				state.previousDays[2].totalAdded += added;

				if (added >= settings.watering.bufferSize[state.watering.currentSolenoid]) {
					//Serial.print("added ");
					//Serial.print(added);
					//Serial.print("sec water to solenoid ");
					//Serial.println(state.watering.currentSolenoid);

					state.watering.finishTime = state.time.getUnixTime() + added;

					haxSolenoidSet(state.watering.currentSolenoid, true);
				}
			}
		}



		// fertiliser sequence events

		if (timeIs(0, 16, 0, 0) && state.automation.fertilise) {
			state.fertilising.startTime = state.time.getUnixTime();
		}

		if (state.time.getUnixTime() == state.fertilising.startTime) {
			safeRelayWrite(pins.fertiliser.tapWater, HIGH);
		}

		if (state.time.getUnixTime() == state.fertilising.startTime + 32) {
			safeRelayWrite(pins.fertiliser.tapWater, LOW);
			safeRelayWrite(pins.fertiliser.pump, HIGH);
			safeRelayWrite(pins.fertiliser.fertiliser, HIGH);
		}

		if (state.time.getUnixTime() == state.fertilising.startTime + 40) {
			safeRelayWrite(pins.fertiliser.tapWater, HIGH);
			safeRelayWrite(pins.fertiliser.pump, LOW);
			safeRelayWrite(pins.fertiliser.fertiliser, LOW);
		}

		if (state.time.getUnixTime() == state.fertilising.startTime + 72) {
			safeRelayWrite(pins.fertiliser.tapWater, LOW);

			state.fertilising.currentSolenoid = -1;

			state.fertilising.startTime = 0;
		}

		// go through each solenoid and turn it on for however long it is set to be, then repeat this process an additional two times
		if (state.fertilising.currentSolenoid >= -1 && state.fertilising.finishTime <= state.time.getUnixTime()) {
			if (state.fertilising.currentSolenoid >= 0) {
				haxSolenoidSet(state.fertilising.currentSolenoid, false);
			}

			state.fertilising.currentSolenoid++;

			if (state.fertilising.currentSolenoid >= sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read)) {
				state.fertilising.currentSolenoid = -1;
				state.fertilising.finishTime = 0;
				state.fertilising.dumpCount++;

				if (state.fertilising.dumpCount == 3) {
					state.fertilising.currentSolenoid = -2;
					state.fertilising.dumpCount = 0;
				}
			} else if (settings.fertilising.fertTimes[state.fertilising.currentSolenoid] > 0) {
				state.fertilising.finishTime = state.time.getUnixTime() + settings.fertilising.fertTimes[state.fertilising.currentSolenoid];
				haxSolenoidSet(state.fertilising.currentSolenoid, true);
			}
		}



		for (int i = 0; i < sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read); i++) {
			if (state.haxTimers.haxTimerState[i] > 1) {
				if (state.haxTimers.triggers[i] == state.time.getUnixTime()) {
					state.haxTimers.isOn[i] = !state.haxTimers.isOn[i];
					haxSolenoidSet(i, state.haxTimers.isOn[i]);

					switch (state.haxTimers.haxTimerState[i]) {
						case 4:
						case 5:
							if (state.haxTimers.isOn[i]) {
								state.haxTimers.triggers[i] += state.haxTimers.haxTimerState[i] == 4 ? 600 : 1200;
							} else {
								state.haxTimers.triggers[i] += 604800;
								state.haxTimers.triggers[i] -= state.haxTimers.haxTimerState[i] == 4 ? 600 : 1200;
							}
							break;
						default:
							state.haxTimers.triggers[i] = 0;
							state.haxTimers.haxTimerState[i] = 0;
					}
				}
			}
		}



		state.firstLoop = false;

		state.lastSecond = state.time.getSecond();
	}



	const struct {
		const int8_t x = JSAxis(analogRead(pins.menuJoystick.x));
		const int8_t y = JSAxis(analogRead(pins.menuJoystick.y));
		const bool button = analogRead(pins.menuJoystick.button) < settings.menuJoystick.buttonSensitivity;
	} menuJoystick;

	if (menuJoystick.x != 0 && state.menuJoystick.x == 0) {
		state.menu.changed = true;
		state.menu.mode += menuJoystick.x;
		const int mmc = softwareConstants.menu.modeCount + sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read);
		if (state.menu.mode >= mmc) {
			state.menu.mode = 0;
		} else if (state.menu.mode < 0) {
			state.menu.mode = mmc - 1;
		}
		state.menu.level = 0;
	}

	if (menuJoystick.y != 0 && state.menuJoystick.y == 0) {
		state.menu.changed = true;
		state.menu.level += menuJoystick.y;
	}

	const bool buttonActive = menuJoystick.button && menuJoystick.x == 0 && menuJoystick.y == 0;

	if (buttonActive && !state.menuJoystick.button) {
		state.menu.changed = true;
		state.menu.activate = true;
	}

	if (menuJoystick.button != state.menuJoystick.button) {
		modules.oled.fillRect(124, 60, 3, 3, buttonActive ? WHITE : BLACK);
		modules.oled.display();
	}

	if (state.menu.changed) {
		const bool toggle = state.menu.level % 2 == 0;

		modules.oled.fillRect(0, 0, 124, 64, BLACK);
		modules.oled.setCursor(0, 0);

		switch (state.menu.mode) {
			case 0:
				if (state.menu.level < 0) {
					state.menu.level = softwareConstants.menu.mainScreenLevels;
				} else if (state.menu.level > softwareConstants.menu.mainScreenLevels) {
					state.menu.level = 0;
				}
				switch (state.menu.level) {
					case 0:
						modules.oled.print(softwareConstants.daysOfTheWeek[(int)state.time.getWeekDay()]);
						modules.oled.print(" ");
						if (state.time.getHour() < 10) {
							modules.oled.print(" ");
						}
						modules.oled.print(state.time.getHour());
						modules.oled.print(":");
						if (state.time.getMinute() < 10) {
							modules.oled.print("0");
						}
						modules.oled.println(state.time.getMinute());

						modules.oled.print("W ");
						modules.oled.print(waterTankLevel(state.previousDays[2].waterTankLevel));
						modules.oled.print("% -");
						// print the total predicted amount to be watered tomorrow, in minutes, rounded up
						{
							unsigned int totalWater = 0;
							for (int i = 0; i < sizeof(pins.haxSolenoid.read) / sizeof(*pins.haxSolenoid.read); i++) {
								totalWater += calcWater(2, i);
							}
							modules.oled.print(waterToMins(totalWater));
						};
						modules.oled.println("m");

						modules.oled.print("R ");
						modules.oled.print(flipsToMils(state.previousDays[2].rainFlips));
						modules.oled.println("mm");

						modules.oled.print("T ");
						modules.oled.print(modules.tempSensor.temperature);
						modules.oled.write(0xF8);
						modules.oled.print("C");
						break;
					case 1:
						modules.oled.println("LAST W %");
						for (int i = 0; i < 3; i++) {
							if (state.previousDays[i].day >= 0) {
								modules.oled.print(softwareConstants.daysOfTheWeek[state.previousDays[i].day]);
								modules.oled.print(" ");
								modules.oled.println(waterTankLevel(state.previousDays[i].waterTankLevel));
							}
						}
						break;
					case 2:
						modules.oled.println("LAST R mm");
						for (int i = 0; i < 3; i++) {
							if (state.previousDays[i].day >= 0) {
								modules.oled.print(softwareConstants.daysOfTheWeek[state.previousDays[i].day]);
								modules.oled.print(" ");
								modules.oled.println(flipsToMils(state.previousDays[i].rainFlips));
							}
						}
						break;
					case 3:
						modules.oled.print("MAX T ");
						modules.oled.write(0xF8);
						modules.oled.println("C");
						for (int i = 0; i < 3; i++) {
							if (state.previousDays[i].day >= 0) {
								modules.oled.print(softwareConstants.daysOfTheWeek[state.previousDays[i].day]);
								modules.oled.print(" ");
								modules.oled.println(state.previousDays[i].maxTemp);
							}
						}
						break;
					case 4:
						modules.oled.println("WATER min");
						for (int i = 0; i < 3; i++) {
							if (state.previousDays[i].day >= 0) {
								modules.oled.print(softwareConstants.daysOfTheWeek[state.previousDays[i].day]);
								modules.oled.print(" ");
								modules.oled.println(waterToMins(state.previousDays[i].totalAdded));
							}
						}
						break;
				}
				break;
			case 1:
				modules.oled.println("AUTO WATER");
				modules.oled.print("NOW ");
				modules.oled.println(state.automation.water ? "ON" : "OFF");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					state.automation.water = toggle;
					updatePersistentMemory();
				}
				break;
			case 2:
				modules.oled.println("AUTO FERT");
				modules.oled.print("NOW ");
				modules.oled.println(state.automation.fertilise ? "ON" : "OFF");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					state.automation.fertilise = toggle;
					updatePersistentMemory();
				}
				break;
			case 3:
				modules.oled.println("AUTO PUMP");
				modules.oled.print("NOW ");
				modules.oled.println(state.automation.fertPump ? "ON" : "OFF");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					state.automation.fertPump = toggle;
					updatePersistentMemory();
				}
				break;
			case 4:
				modules.oled.println("PUMP");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					safeRelayWrite(pins.fertiliser.pump, toggle);
				}
				break;
			case 5:
				modules.oled.println("FERTSOL");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					safeRelayWrite(pins.fertiliser.fertiliser, toggle);
				}
				break;
			case 6:
				modules.oled.println("FRESH");
				modules.oled.print(toggle ? "ON" : "OFF");
				if (state.menu.activate) {
					safeRelayWrite(pins.fertiliser.tapWater, toggle);
				}
				break;
			case 7:
				modules.oled.println("FERTILISE");
				modules.oled.println(state.fertilising.startTime ? "ACTIVE" : "INACTIVE");
				if (state.menu.activate && !state.fertilising.startTime) {
					state.fertilising.startTime = state.time.getUnixTime() + 1;
				}
				break;
			case 8:
				modules.oled.println("FERTOUT");

				if (state.menu.level < 2) {
					state.menu.level = 2;
				}

				modules.oled.print(state.menu.level * 10);
				modules.oled.println(" mL");

				if (state.menu.activate) {
					safeRelayWrite(pins.fertiliser.pump, true);

					delay(120000);

					safeRelayWrite(pins.fertiliser.pump, false);

					delay(500);

					safeRelayWrite(pins.fertiliser.fertiliser, true);

					delay(500);

					safeRelayWrite(pins.fertiliser.pump, true);

					delay(state.menu.level * 10 * 33 - 492);

					safeRelayWrite(pins.fertiliser.pump, false);
					safeRelayWrite(pins.fertiliser.fertiliser, false);
				}
				break;

			default:
				{
					const int hxs = state.menu.mode - softwareConstants.menu.modeCount;

					modules.oled.print("HXS ");
					modules.oled.println(hxs);

					modules.oled.print("NOW ");

					if (state.menu.level < 0) {
						state.menu.level = 5;
					} else if (state.menu.level > 5) {
						state.menu.level = 0;
					}

					switch (state.haxTimers.haxTimerState[hxs]) {
						case 0:
							modules.oled.println("OFF");
							break;
						case 1:
							modules.oled.println("ON");
							break;
						case 2:
							modules.oled.println("10");
							break;
						case 3:
							modules.oled.println("20");
							break;
						case 4:
							modules.oled.println("10W");
							break;
						case 5:
							modules.oled.println("20W");
							break;
					}

					if (state.menu.activate) {
						state.haxTimers.haxTimerState[hxs] = state.menu.level;
					}

					switch (state.menu.level) {
						case 0:
							modules.oled.print("OFF");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = 0;
								state.haxTimers.isOn[hxs] = false;
								haxSolenoidSet(hxs, false);
							}
							break;
						case 1:
							modules.oled.print("ON");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = 0;
								state.haxTimers.isOn[hxs] = true;
								haxSolenoidSet(hxs, true);
							}
							break;
						case 2:
							modules.oled.print("10");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = state.time.getUnixTime() + 600;
								state.haxTimers.isOn[hxs] = true;
								haxSolenoidSet(hxs, true);
							}
							break;
						case 3:
							modules.oled.print("20");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = state.time.getUnixTime() + 1200;
								state.haxTimers.isOn[hxs] = true;
								haxSolenoidSet(hxs, true);
							}
							break;
						case 4:
							modules.oled.print("10W");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = state.time.getUnixTime() + 600;
								state.haxTimers.isOn[hxs] = true;
								haxSolenoidSet(hxs, true);
							}
							break;
						case 5:
							modules.oled.print("20W");

							if (state.menu.activate) {
								state.haxTimers.triggers[hxs] = state.time.getUnixTime() + 1200;
								state.haxTimers.isOn[hxs] = true;
								haxSolenoidSet(hxs, true);
							}
							break;
					}
				};
		}

		modules.oled.display();
	}

	state.menuJoystick.x = menuJoystick.x;
	state.menuJoystick.y = menuJoystick.y;
	state.menuJoystick.button = menuJoystick.button;

	state.menu.changed = state.menu.activate;
	state.menu.activate = false;
}



// function for reading joystick movements in a discrete manner
int8_t JSAxis(int x) {
	if (x < settings.menuJoystick.joystickSensitivity) {
		return -1;
	}
	if (x > 1023 - settings.menuJoystick.joystickSensitivity) {
		return 1;
	}
	return 0;
}

// returns true if the first value is negative or if the two values are equal, otherwise false
bool timeCheck(int x, int v) {
	return x < 0 || x == v;
}

// returns true if each value either matches the time in t or is negative
// e.g. timeIs(t, -1, 8, 0, 0) is true regardless of what day of the week it is but only at precisely 8:00:00 AM each day
// days of the week are 1 (Sunday), 2 (Monday), ..., 7 (Saturday)
bool timeIs(int weekDay, int hour, int min, int sec) {
	return timeCheck(weekDay, state.time.getWeekDay()) && timeCheck(hour, state.time.getHour()) && timeCheck(min, state.time.getMinute()) && timeCheck(sec, state.time.getSecond());
}

void haxSolenoidSet(int hxs, bool newState) {
	state.haxSolenoidOn.oldState[hxs] = -1;
	state.haxSolenoidOn.desiredState[hxs] = newState;
}

// time the ultrasonic sensor echo pulse
void waterTankUltrasonicEcho() {
	if (state.noPinInterrupts) {
		return;
	}

	if (digitalRead(pins.waterTankUltrasonic.echo)) {
		state.waterTankUltrasonicTimer = micros();
	} else {
		state.previousDays[2].waterTankLevel = micros() - state.waterTankUltrasonicTimer;
	}
}

// calculate water tank percentage from ultrasonic sensor pulse length (microseconds)
int waterTankLevel(unsigned long time) {
	return 100 - round(1.715 * time / softwareConstants.waterTankHeight);
}

// convert rain sensor flips to millimetres of rain
float flipsToMils(int flips) {
	return 0.45 * flips;
}

// disable certain interrupts while certain relays are being switched so that weird stuff doesn't happen
void safeRelayWrite(int pin, bool pinState) {
	state.noPinInterrupts = 2;
	digitalWrite(pin, pinState);
}

bool getEEPROMBit(int addr, int bit) {
	return EEPROM.read(addr) & 1 << bit;
}

void updatePersistentMemory() {
	EEPROM.update(softwareConstants.persistentMemory.automationSwitches, char(state.automation.fertilise + state.automation.water * 2 + state.automation.fertPump * 4));
}

unsigned int calcWater(int day, int line) {
	const float deltaWater = min(settings.watering.maxRain, state.previousDays[day].rainFlips) * settings.watering.rainCoeff + state.previousDays[day].maxTemp * settings.watering.tempCoeff;

	const float current = state.watering.tallies[line] + deltaWater;

	return max(0, round(settings.watering.desiredLevels[line] - current));
}

unsigned int waterToMins(unsigned int water) {
	return water / 60 + (water % 60 ? 1 : 0);
}
