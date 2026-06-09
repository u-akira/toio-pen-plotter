#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

#include "config.h"

namespace
{

  constexpr uint16_t SCREEN_WIDTH = 128;
  constexpr uint16_t SCREEN_HEIGHT = 64;
  constexpr uint8_t OLED_ADDR = 0x3C;
  constexpr uint8_t TOIO_SCAN_SECONDS = 5;
  constexpr uint16_t BUTTON_DEBOUNCE_MS = 30;
  constexpr uint16_t LONG_PRESS_MS = 500;
  constexpr uint16_t TOIO2_PULSE_MS = 150;

  constexpr char TOIO_SERVICE_UUID[] = "10B20100-5B3B-4571-9508-CF3EFCD7BBAE";
  constexpr char TOIO_MOTOR_UUID[] = "10B20102-5B3B-4571-9508-CF3EFCD7BBAE";
  constexpr char TOIO_INDICATOR_UUID[] = "10B20103-5B3B-4571-9508-CF3EFCD7BBAE";
  constexpr char TOIO_SOUND_UUID[] = "10B20104-5B3B-4571-9508-CF3EFCD7BBAE";

  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
  Adafruit_NeoPixel pixels(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

  enum class AppMode : uint8_t
  {
    Idle,
    Play,
  };

  enum class UiState : uint8_t
  {
    Idle,
    Connecting,
    Connected,
    Play,
  };

  enum class CubeSlot : uint8_t
  {
    Slot1 = 0,
    Slot2 = 1,
  };

  enum class MotorIntent : uint8_t
  {
    Stop,
    Forward,
    Backward,
  };

  struct ButtonState
  {
    uint8_t pin;
    bool activeLow;
    bool stablePressed = false;
    bool rawPressed = false;
    uint32_t lastChangeMs = 0;
    uint32_t pressedAtMs = 0;
    bool longPressFired = false;
    bool clickFired = false;

    void begin()
    {
      pinMode(pin, INPUT_PULLUP);
      rawPressed = readRaw();
      stablePressed = rawPressed;
      lastChangeMs = millis();
      pressedAtMs = stablePressed ? millis() : 0;
      longPressFired = false;
      clickFired = false;
    }

    bool readRaw() const
    {
      bool value = digitalRead(pin) == LOW;
      return activeLow ? value : !value;
    }

    void update()
    {
      const bool currentRaw = readRaw();
      const uint32_t now = millis();
      if (currentRaw != rawPressed)
      {
        rawPressed = currentRaw;
        lastChangeMs = now;
      }

      if (currentRaw != stablePressed && (now - lastChangeMs) >= BUTTON_DEBOUNCE_MS)
      {
        stablePressed = currentRaw;
        if (stablePressed)
        {
          pressedAtMs = now;
          longPressFired = false;
          clickFired = false;
        }
      }
    }

    bool pressed() const { return stablePressed; }

    bool justPressedForLong(uint32_t now)
    {
      return stablePressed && !longPressFired && (now - pressedAtMs) >= LONG_PRESS_MS;
    }

    bool justReleased() const
    {
      return !stablePressed && pressedAtMs != 0;
    }

    uint32_t pressDuration(uint32_t now) const
    {
      return stablePressed ? (now - pressedAtMs) : 0;
    }

    void markLongPressHandled()
    {
      longPressFired = true;
    }

    void clearReleaseLatch()
    {
      pressedAtMs = 0;
      longPressFired = false;
      clickFired = false;
    }
  };

  struct ToioCube
  {
    String shortName;
    String address;
    bool connected = false;
    NimBLEClient *client = nullptr;
    NimBLERemoteCharacteristic *motorChar = nullptr;
    NimBLERemoteCharacteristic *indicatorChar = nullptr;
    NimBLERemoteCharacteristic *soundChar = nullptr;
    MotorIntent activeIntent = MotorIntent::Stop;
    int8_t activeSpeed = 0;
    uint32_t pulseUntilMs = 0;

    void reset()
    {
      shortName = "";
      address = "";
      connected = false;
      motorChar = nullptr;
      indicatorChar = nullptr;
      soundChar = nullptr;
      activeIntent = MotorIntent::Stop;
      activeSpeed = 0;
      pulseUntilMs = 0;
      if (client != nullptr)
      {
        if (client->isConnected())
        {
          client->disconnect();
        }
        NimBLEDevice::deleteClient(client);
        client = nullptr;
      }
    }

    bool ready() const
    {
      return connected && motorChar != nullptr;
    }
  };

  struct ToioCandidate
  {
    NimBLEAddress address;
    String addressText;
    String shortName;
    uint8_t addressType = 0;
    uint8_t advType = 0;
    int rssi = -999;
    bool connectable = false;
  };

  AppMode appMode = AppMode::Idle;
  UiState uiState = UiState::Idle;
  ToioCube cubes[2];
  String statusLine = "";
  uint32_t statusUntilMs = 0;
  String connectDebugLine = "";

  ButtonState btnUp{BUTTON_UP, true};
  ButtonState btnDown{BUTTON_DOWN, true};
  ButtonState btnLeft{BUTTON_LEFT, true};
  ButtonState btnRight{BUTTON_RIGHT, true};

  bool bluetoothReady = false;

  String slotLabel(CubeSlot slot)
  {
    return slot == CubeSlot::Slot1 ? "toio1" : "toio2";
  }

  void setStatus(const String &text, uint32_t durationMs = 1800)
  {
    statusLine = text;
    statusUntilMs = millis() + durationMs;
  }

  void setPersistentStatus(const String &text)
  {
    statusLine = text;
    statusUntilMs = 0;
  }

  void beep(uint16_t freq = 1800, uint16_t durationMs = 35)
  {
    tone(BUZZER_PIN, freq, durationMs);
  }

  bool canWriteToioCharacteristic(NimBLERemoteCharacteristic *characteristic)
  {
    return characteristic != nullptr && (characteristic->canWriteNoResponse() || characteristic->canWrite());
  }

  void setCubeIndicator(ToioCube &cube, uint8_t r, uint8_t g, uint8_t b)
  {
    if (cube.indicatorChar == nullptr)
    {
      return;
    }

    uint8_t data[] = {
        0x03, // indicator on/off
        0x00, // duration: keep until next write
        0x01, // one indicator
        0x01, // indicator id
        r,
        g,
        b,
    };
    cube.indicatorChar->writeValue(data, sizeof(data), false);
  }

  void playCubeSoundEffect(ToioCube &cube, uint8_t soundEffectId, uint8_t volume = 0xFF)
  {
    if (cube.soundChar == nullptr)
    {
      return;
    }

    uint8_t data[] = {0x02, soundEffectId, volume};
    cube.soundChar->writeValue(data, sizeof(data), false);
  }

  void setPixels(uint8_t r, uint8_t g, uint8_t b)
  {
    for (uint8_t i = 0; i < pixels.numPixels(); ++i)
    {
      pixels.setPixelColor(i, pixels.Color(r, g, b));
    }
    pixels.show();
  }

  void showIdlePixels()
  {
    setPixels(0, 0, 0);
  }

  void showConnectedPixels()
  {
    setPixels(0, 0, 0);
    if (cubes[0].connected && pixels.numPixels() >= 1)
    {
      pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    }
    if (cubes[1].connected && pixels.numPixels() >= 2)
    {
      pixels.setPixelColor(1, pixels.Color(0, 0, 255));
    }
    pixels.show();
  }

  void showPlayPixels()
  {
    showConnectedPixels();
  }

  String shortNameFromDevice(const NimBLEAdvertisedDevice &device)
  {
    if (device.haveName())
    {
      String name = device.getName().c_str();
      if (name.length() > 0)
      {
        return name;
      }
    }
    return device.getAddress().toString().c_str();
  }

  void printBleLogPrefix(const char *event)
  {
    Serial.print("[");
    Serial.print(millis());
    Serial.print("] BLE ");
    Serial.print(event);
    Serial.print(": ");
  }

  void printAdvertisedDeviceSummary(const NimBLEAdvertisedDevice &device)
  {
    Serial.print("addr=");
    Serial.print(device.getAddress().toString().c_str());
    Serial.print(" name=");
    Serial.print(shortNameFromDevice(device));
    Serial.print(" rssi=");
    Serial.print(device.getRSSI());
    Serial.print(" addrType=");
    Serial.print(device.getAddressType());
    Serial.print(" advType=");
    Serial.print(device.getAdvType());
    Serial.print(" connectable=");
    Serial.print(device.isConnectable() ? "yes" : "no");
    Serial.print(" service=");
    Serial.print(device.haveServiceUUID() ? "yes" : "no");
  }

  void printCandidateSummary(const ToioCandidate &candidate)
  {
    Serial.print("addr=");
    Serial.print(candidate.addressText);
    Serial.print(" name=");
    Serial.print(candidate.shortName);
    Serial.print(" rssi=");
    Serial.print(candidate.rssi);
    Serial.print(" addrType=");
    Serial.print(candidate.addressType);
    Serial.print(" advType=");
    Serial.print(candidate.advType);
    Serial.print(" connectable=");
    Serial.print(candidate.connectable ? "yes" : "no");
  }

  ToioCandidate candidateFromDevice(const NimBLEAdvertisedDevice &device)
  {
    ToioCandidate candidate;
    candidate.addressText = device.getAddress().toString().c_str();
    candidate.shortName = shortNameFromDevice(device);
    candidate.addressType = device.getAddressType();
    candidate.advType = device.getAdvType();
    candidate.rssi = device.getRSSI();
    candidate.connectable = device.isConnectable();
    candidate.address = NimBLEAddress(candidate.addressText.c_str(), candidate.addressType);
    return candidate;
  }

  bool isToioCube(const NimBLEAdvertisedDevice &device)
  {
    if (device.haveServiceUUID() && device.isAdvertisingService(NimBLEUUID(TOIO_SERVICE_UUID)))
    {
      return true;
    }
    if (device.haveName())
    {
      String name = device.getName().c_str();
      return name.startsWith("toio");
    }
    return false;
  }

  int findFreeSlot()
  {
    if (!cubes[0].connected)
      return 0;
    if (!cubes[1].connected)
      return 1;
    return -1;
  }

  int findSlotByAddress(const String &address)
  {
    for (int i = 0; i < 2; ++i)
    {
      if (cubes[i].connected && cubes[i].address == address)
      {
        return i;
      }
    }
    return -1;
  }

  int connectedCubeCount()
  {
    int count = 0;
    for (const auto &cube : cubes)
    {
      if (cube.connected)
      {
        ++count;
      }
    }
    return count;
  }

  void enterPlayMode(bool showStatus = true);

  void drawDisplay()
  {
    display.clearDisplay();
    display.setTextWrap(false);
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Mode: ");
    if (uiState == UiState::Connecting)
    {
      display.print("CONNECTING");
    }
    else if (uiState == UiState::Connected)
    {
      display.print("CONNECTED");
    }
    else if (uiState == UiState::Play)
    {
      display.print("PLAY");
    }
    else
    {
      display.print("IDLE");
    }

    display.setCursor(0, 10);
    display.print("toio1: ");
    display.print(cubes[0].connected ? cubes[0].shortName : String("-"));

    display.setCursor(0, 20);
    display.print("toio2: ");
    display.print(cubes[1].connected ? cubes[1].shortName : String("-"));

    display.setCursor(0, 32);
    if (uiState == UiState::Connecting)
    {
      display.print("Scanning toio...");
      display.setCursor(0, 42);
      display.print("Please wait");
    }
    else if (uiState == UiState::Play)
    {
      display.print("1: Hold UP/DN/L/R");
      display.setCursor(0, 42);
      display.print("2: Tap UP/DN");
    }
    else if (connectedCubeCount() == 0)
    {
      display.print("UP: Connect");
    }
    else if (connectedCubeCount() == 1)
    {
      display.print("UP: Connect");
      display.setCursor(0, 42);
      display.print("DOWN: Play");
    }

    display.setCursor(0, 54);
    display.print(statusLine);
    if (statusLine.length() == 0 && connectDebugLine.length() > 0)
    {
      display.print(connectDebugLine);
    }

    display.display();
  }

  void stopMotor(ToioCube &cube)
  {
    if (!cube.ready())
    {
      return;
    }
    uint8_t data[] = {0x01, 0x01, 0x01, 0x00, 0x02, 0x01, 0x00};
    cube.motorChar->writeValue(data, sizeof(data), false);
    cube.activeIntent = MotorIntent::Stop;
    cube.activeSpeed = 0;
    cube.pulseUntilMs = 0;
  }

  void writeMotorCommand(ToioCube &cube, int8_t leftSpeed, int8_t rightSpeed, bool timed = false, uint8_t durationUnits = 0)
  {
    if (!cube.ready())
    {
      return;
    }

    auto encodeMotor = [](int8_t speed, uint8_t &direction, uint8_t &commandSpeed)
    {
      if (speed > 0)
      {
        direction = 0x01;
        commandSpeed = static_cast<uint8_t>(speed);
      }
      else if (speed < 0)
      {
        direction = 0x02;
        commandSpeed = static_cast<uint8_t>(-speed);
      }
      else
      {
        direction = 0x01;
        commandSpeed = 0;
      }
    };

    uint8_t leftDir = 0x01;
    uint8_t leftVal = 0;
    uint8_t rightDir = 0x01;
    uint8_t rightVal = 0;
    encodeMotor(leftSpeed, leftDir, leftVal);
    encodeMotor(rightSpeed, rightDir, rightVal);

    uint8_t data[8] = {
        timed ? 0x02 : 0x01,
        0x01,
        leftDir,
        leftVal,
        0x02,
        rightDir,
        rightVal,
        timed ? durationUnits : 0x00};

    const size_t length = timed ? sizeof(data) : 7;
    cube.motorChar->writeValue(data, length, false);
  }

  void driveCube(ToioCube &cube, int8_t speed, bool timed = false, uint8_t durationUnits = 0)
  {
    if (speed == 0)
    {
      stopMotor(cube);
      return;
    }
    writeMotorCommand(cube, speed, speed, timed, durationUnits);
    cube.activeIntent = speed > 0 ? MotorIntent::Forward : MotorIntent::Backward;
    cube.activeSpeed = speed;
    cube.pulseUntilMs = timed ? (millis() + (uint32_t)durationUnits * 10UL) : 0;
  }

  void driveTurn(ToioCube &cube, int8_t leftSpeed, int8_t rightSpeed)
  {
    if (!cube.ready())
    {
      return;
    }
    writeMotorCommand(cube, leftSpeed, rightSpeed, false, 0);
    cube.activeIntent = MotorIntent::Stop;
    cube.activeSpeed = 0;
    cube.pulseUntilMs = 0;
  }

  bool connectCubeSlot(int slotIndex, const ToioCandidate &target)
  {
    if (slotIndex < 0 || slotIndex > 1)
    {
      printBleLogPrefix("connect");
      Serial.println("invalid slot");
      return false;
    }

    ToioCube &cube = cubes[slotIndex];
    cube.reset();

    printBleLogPrefix("connect");
    Serial.print("slot=");
    Serial.print(slotIndex + 1);
    Serial.print(" target ");
    printCandidateSummary(target);
    Serial.println();

    bool connected = false;
    NimBLEClient *client = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
      client = NimBLEDevice::createClient();
      if (client == nullptr)
      {
        printBleLogPrefix("connect");
        Serial.println("createClient failed");
        connectDebugLine = "createClient failed";
        return false;
      }
      printBleLogPrefix("connect");
      Serial.println("client created");

      printBleLogPrefix("connect");
      Serial.print("attempt ");
      Serial.print(attempt + 1);
      Serial.println(" start");
      if (client->connect(target.address, true, false, false))
      {
        connected = true;
        printBleLogPrefix("connect");
        Serial.print("attempt ");
        Serial.print(attempt + 1);
        Serial.println(" success");
        break;
      }
      printBleLogPrefix("connect");
      Serial.print("attempt ");
      Serial.print(attempt + 1);
      Serial.print(" failed for ");
      Serial.println(target.addressText);
      if (client->isConnected())
      {
        client->disconnect();
      }
      NimBLEDevice::deleteClient(client);
      client = nullptr;
      delay(1000);
    }
    if (!connected)
    {
      printBleLogPrefix("connect");
      Serial.print("all attempts failed for ");
      Serial.println(target.addressText);
      connectDebugLine = "connect failed";
      NimBLEDevice::deleteClient(client);
      uiState = UiState::Idle;
      return false;
    }

    printBleLogPrefix("gatt");
    Serial.println("discover toio service");
    NimBLERemoteService *service = client->getService(NimBLEUUID(TOIO_SERVICE_UUID));
    if (service == nullptr)
    {
      printBleLogPrefix("gatt");
      Serial.print("toio service missing for ");
      Serial.println(target.addressText);
      connectDebugLine = "service missing";
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      uiState = UiState::Idle;
      return false;
    }
    printBleLogPrefix("gatt");
    Serial.println("toio service found");

    printBleLogPrefix("gatt");
    Serial.println("discover motor characteristic");
    NimBLERemoteCharacteristic *motorChar = service->getCharacteristic(NimBLEUUID(TOIO_MOTOR_UUID));
    if (!canWriteToioCharacteristic(motorChar))
    {
      printBleLogPrefix("gatt");
      Serial.print("motor characteristic missing or not writable for ");
      Serial.println(target.addressText);
      connectDebugLine = "motor missing";
      client->disconnect();
      NimBLEDevice::deleteClient(client);
      uiState = UiState::Idle;
      return false;
    }
    printBleLogPrefix("gatt");
    Serial.print("motor characteristic writable write=");
    Serial.print(motorChar->canWrite() ? "yes" : "no");
    Serial.print(" writeNoResponse=");
    Serial.println(motorChar->canWriteNoResponse() ? "yes" : "no");

    NimBLERemoteCharacteristic *indicatorChar = service->getCharacteristic(NimBLEUUID(TOIO_INDICATOR_UUID));
    NimBLERemoteCharacteristic *soundChar = service->getCharacteristic(NimBLEUUID(TOIO_SOUND_UUID));
    printBleLogPrefix("gatt");
    Serial.print("indicator=");
    Serial.print(indicatorChar != nullptr ? "found" : "missing");
    Serial.print(" sound=");
    Serial.println(soundChar != nullptr ? "found" : "missing");

    cube.client = client;
    cube.motorChar = motorChar;
    cube.indicatorChar = indicatorChar;
    cube.soundChar = soundChar;
    cube.connected = true;
    cube.shortName = target.shortName;
    cube.address = target.addressText;
    cube.activeIntent = MotorIntent::Stop;
    cube.activeSpeed = 0;
    cube.pulseUntilMs = 0;

    if (slotIndex == static_cast<int>(CubeSlot::Slot2))
    {
      setCubeIndicator(cube, 0, 0, 255);
    }
    else
    {
      setCubeIndicator(cube, 0, 255, 0);
    }

    setPersistentStatus(String("Connected ") + slotLabel(static_cast<CubeSlot>(slotIndex)) + ": " + cube.shortName);
    connectDebugLine = "";
    printBleLogPrefix("connect");
    Serial.print("connected ");
    Serial.print(slotLabel(static_cast<CubeSlot>(slotIndex)));
    Serial.print(": ");
    Serial.print(cube.shortName);
    Serial.print(" addr=");
    Serial.println(cube.address);
    playCubeSoundEffect(cube, 0x04);
    beep(2000, 30);
    uiState = UiState::Connected;
    return true;
  }

  bool connectNextCube()
  {
    const int freeSlot = findFreeSlot();
    if (freeSlot < 0)
    {
      setStatus("Already connected 2 cubes");
      printBleLogPrefix("scan");
      Serial.println("skip: both slots already connected");
      return false;
    }

    uiState = UiState::Connecting;
    setStatus("Scanning...");
    drawDisplay();
    printBleLogPrefix("scan");
    Serial.print("start seconds=");
    Serial.print(TOIO_SCAN_SECONDS);
    Serial.print(" freeSlot=");
    Serial.println(freeSlot + 1);

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(15);

    bool connected = false;
    bool sawToio = false;
    bool sawConnectableToio = false;
    bool hasCandidate = false;
    ToioCandidate selectedCandidate;
    NimBLEScanResults results = scan->getResults(static_cast<uint32_t>(TOIO_SCAN_SECONDS) * 1000UL, false);
    printBleLogPrefix("scan");
    Serial.print("complete count=");
    Serial.println(results.getCount());
    for (int i = 0; i < results.getCount(); ++i)
    {
      const NimBLEAdvertisedDevice *foundDevice = results.getDevice(i);
      if (foundDevice == nullptr)
      {
        continue;
      }
      const NimBLEAdvertisedDevice &device = *foundDevice;
      if (!isToioCube(device))
      {
        continue;
      }
      sawToio = true;
      printBleLogPrefix("scan");
      Serial.print("toio candidate ");
      printAdvertisedDeviceSummary(device);
      if (findSlotByAddress(device.getAddress().toString().c_str()) >= 0)
      {
        Serial.println(" alreadyConnected=yes");
        continue;
      }
      if (!device.isConnectable())
      {
        Serial.println(" alreadyConnected=no skipped=not-connectable");
        continue;
      }
      sawConnectableToio = true;
      Serial.println(" alreadyConnected=no");
      if (!hasCandidate || device.getRSSI() > selectedCandidate.rssi)
      {
        selectedCandidate = candidateFromDevice(device);
        hasCandidate = true;
      }
    }
    scan->clearResults();

    if (hasCandidate)
    {
      printBleLogPrefix("scan");
      Serial.print("selected ");
      printCandidateSummary(selectedCandidate);
      Serial.println();
      scan->stop();
      delay(1000);
      connected = connectCubeSlot(freeSlot, selectedCandidate);
    }
    else
    {
      printBleLogPrefix("scan");
      Serial.println(sawToio ? "no unconnected connectable toio candidate" : "no toio candidate");
    }

    if (!connected)
    {
      if (!sawToio)
      {
        setStatus("No free toio found");
        connectDebugLine = "no toio in scan";
      }
      else if (!sawConnectableToio)
      {
        setStatus("toio not connectable");
        connectDebugLine = "not connectable";
      }
      else
      {
        setStatus("Found toio, connect failed");
        connectDebugLine = "connect failed";
      }
      uiState = (connectedCubeCount() > 0) ? UiState::Connected : UiState::Idle;
      printBleLogPrefix("connect");
      Serial.print("result=failed connectedCount=");
      Serial.println(connectedCubeCount());
    }
    else
    {
      printBleLogPrefix("connect");
      Serial.print("result=success connectedCount=");
      Serial.println(connectedCubeCount());
    }

    if (cubes[0].connected && cubes[1].connected)
    {
      showConnectedPixels();
    }
    if (connected && connectedCubeCount() >= 2)
    {
      enterPlayMode(true);
    }
    return connected;
  }

  void enterPlayMode(bool showStatus)
  {
    if (connectedCubeCount() < 1)
    {
      setStatus("Need 1 cube");
      return;
    }
    appMode = AppMode::Play;
    uiState = UiState::Play;
    if (showStatus)
    {
      setPersistentStatus("Play mode");
    }
    showPlayPixels();
    stopMotor(cubes[0]);
    stopMotor(cubes[1]);
    if (showStatus)
    {
      Serial.println("Entered play mode");
    }
  }

  void handleIdleButtons()
  {
    if (btnUp.justReleased())
    {
      printBleLogPrefix("button");
      Serial.println("connect button released in idle");
      connectNextCube();
      btnUp.clearReleaseLatch();
    }

    if (btnDown.justReleased())
    {
      if (connectedCubeCount() >= 1)
      {
        enterPlayMode();
      }
      btnDown.clearReleaseLatch();
    }
  }

  void handlePlayButtons()
  {
    const uint32_t now = millis();

    auto handleDualPurposeButton = [&](ButtonState &button, int8_t toio1Speed, int8_t toio2Speed)
    {
      if (button.justPressedForLong(now))
      {
        driveCube(cubes[0], toio1Speed);
        button.markLongPressHandled();
      }

      if (button.justReleased())
      {
        const uint32_t heldMs = button.pressDuration(now);
        if (button.longPressFired)
        {
          stopMotor(cubes[0]);
        }
        else if (heldMs < LONG_PRESS_MS)
        {
          driveCube(cubes[1], toio2Speed, true, static_cast<uint8_t>(TOIO2_PULSE_MS / 10));
        }
        button.clearReleaseLatch();
      }
    };

    handleDualPurposeButton(btnUp, 50, -30);
    handleDualPurposeButton(btnDown, -20, 30);

    if (btnLeft.justPressedForLong(now))
    {
      driveTurn(cubes[0], -30, 30);
      btnLeft.markLongPressHandled();
    }
    if (btnLeft.justReleased())
    {
      if (btnLeft.longPressFired)
      {
        stopMotor(cubes[0]);
      }
      btnLeft.clearReleaseLatch();
    }

    if (btnRight.justPressedForLong(now))
    {
      driveTurn(cubes[0], 30, -30);
      btnRight.markLongPressHandled();
    }
    if (btnRight.justReleased())
    {
      if (btnRight.longPressFired)
      {
        stopMotor(cubes[0]);
      }
      btnRight.clearReleaseLatch();
    }
  }

  void updateMotors()
  {
    const uint32_t now = millis();
    for (auto &cube : cubes)
    {
      if (cube.connected && cube.pulseUntilMs != 0 && now >= cube.pulseUntilMs)
      {
        stopMotor(cube);
      }
    }
  }

  void processSerialCommands()
  {
    while (Serial.available() > 0)
    {
      const char c = static_cast<char>(Serial.read());
      if (c == 'c' || c == 'C')
      {
        if (appMode == AppMode::Idle)
        {
          printBleLogPrefix("serial");
          Serial.println("connect command");
          connectNextCube();
        }
        else
        {
          setStatus("Connect disabled in play");
        }
      }
      else if (c == 'p' || c == 'P')
      {
        enterPlayMode();
      }
      else if (c == 's' || c == 'S')
      {
        stopMotor(cubes[0]);
        stopMotor(cubes[1]);
        setStatus("Stopped");
      }
      else if (c == 'i' || c == 'I')
      {
        appMode = AppMode::Idle;
        uiState = (connectedCubeCount() > 0) ? UiState::Connected : UiState::Idle;
        if (connectedCubeCount() > 0)
        {
          setPersistentStatus("Connected cube info");
          showConnectedPixels();
        }
        else
        {
          setPersistentStatus("Idle");
          showIdlePixels();
        }
      }
    }
  }

  void printStartupHelp()
  {
    Serial.println();
    Serial.println("toio-pen-plotter ready");
    Serial.println("Commands:");
    Serial.println("  c = connect next toio");
    Serial.println("  p = play mode");
    Serial.println("  s = stop motors");
    Serial.println("  i = idle mode");
  }

  void initBluetooth()
  {
    printBleLogPrefix("init");
    Serial.println("NimBLEDevice::init");
    NimBLEDevice::init("toio-pen-plotter");
    NimBLEDevice::setPower(9);
    bluetoothReady = true;
    printBleLogPrefix("init");
    Serial.println("NimBLE ready power=9dBm");
  }

} // namespace

void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(200);

  pinMode(BUZZER_PIN, OUTPUT);
  pixels.begin();
  pixels.clear();
  pixels.show();

  btnUp.begin();
  btnDown.begin();
  btnLeft.begin();
  btnRight.begin();

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("OLED init failed");
  }
  display.clearDisplay();
  display.display();

  initBluetooth();
  uiState = UiState::Idle;
  showIdlePixels();
  setPersistentStatus("Idle");
  connectDebugLine = "";
  printStartupHelp();
  drawDisplay();
}

void loop()
{
  btnUp.update();
  btnDown.update();
  btnLeft.update();
  btnRight.update();

  processSerialCommands();

  if (appMode == AppMode::Idle)
  {
    handleIdleButtons();
  }
  else
  {
    handlePlayButtons();
  }

  updateMotors();

  const uint32_t now = millis();
  if (statusUntilMs != 0 && now >= statusUntilMs)
  {
    statusLine = "";
    statusUntilMs = 0;
  }

  if (!cubes[0].connected && !cubes[1].connected)
  {
    showIdlePixels();
  }
  else if (appMode == AppMode::Play)
  {
    showPlayPixels();
  }
  else
  {
    showConnectedPixels();
  }

  drawDisplay();
  delay(10);
}
