

#include <Arduino.h>

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NimBLEDevice.h>

#include "esp_sntp.h"


const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
const char *mqtt_server = MQTT_SERVER;
const char *mqtt_username = MQTT_USER;
const char *mqtt_pass = MQTT_PASSWORD;
const int mqtt_port = MQTT_PORT;

const char* ntpServer = "192.168.199.33";
const long  gmtOffset_sec = -28800;
const int   daylightOffset_sec = 3600;

#include "main.h"

WiFiClient espClient;
PubSubClient client(espClient);
WiFiServer server(80);


static const NimBLEAdvertisedDevice* advDevice;
static bool                          doConnect  = false;
static uint32_t                      scanTimeMs = 5000; /** scan time in milliseconds, 0 = scan forever */
NimBLERemoteService*        pSvc = nullptr;
NimBLERemoteCharacteristic* pChr = nullptr;
NimBLERemoteService*        writeSvc = nullptr;
NimBLERemoteCharacteristic* writeChr = nullptr;

NimBLERemoteDescriptor*     pDsc = nullptr;

unsigned long previousMillis = 0;

unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  // getLocalTime will wait for the first successful sync
  if (!getLocalTime(&timeinfo)) { // `timeinfo` is a struct tm variable
      Serial.println("Failed to obtain time");
      return 0;
  }
  time(&now); // Fills 'now' with the current Unix timestamp
  return now;
}


void notify(struct timeval* t) {
  Serial.println("synchronized");
}


void initSNTP() {  
  sntp_set_sync_interval(1 * 60 * 60 * 1000UL);  // 1 hour
  sntp_set_time_sync_notification_cb(notify);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "192.168.199.33");
  esp_sntp_init();
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0",1);
  tzset();
}



int hexCharToInt(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1; // Invalid hex character
}

std::vector<uint8_t> hexToAscii(std::string hexString) { 

	std::vector<uint8_t> theints;
    for (size_t i = 0; i < hexString.length(); i += 2) {
        int highNibble = hexCharToInt(hexString[i]);
        int lowNibble = hexCharToInt(hexString[i + 1]);

        if (highNibble == -1 || lowNibble == -1) {
            return theints;
        }

        int number = (highNibble << 4) | lowNibble;
		theints.push_back(number);
    }
    return theints;
}

int calculateTemp(std::vector<uint8_t> z, int i) {

	return (z[i] * 100) + (z[i+1] * 10) + (z[i+2]);

}




/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override { Serial.printf("Connected\n"); }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.printf("%s Disconnected, reason = %d - Starting scan\n", pClient->getPeerAddress().toString().c_str(), reason);
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }

    /********************* Security handled here *********************/
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        Serial.printf("Server Passkey Entry\n");
        /**
         * This should prompt the user to enter the passkey displayed
         * on the peer device.
         */
        NimBLEDevice::injectPassKey(connInfo, 123456);
    }

    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
        Serial.printf("The passkey YES/NO number: %" PRIu32 "\n", pass_key);
        /** Inject false if passkeys don't match. */
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    /** Pairing process complete, we can check the results in connInfo */
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (!connInfo.isEncrypted()) {
            Serial.printf("Encrypt connection failed - disconnecting\n");
            /** Find the client with the connection handle provided in connInfo */
            NimBLEDevice::getClientByHandle(connInfo.getConnHandle())->disconnect();
            return;
        }
    }
} clientCallbacks;


class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        Serial.printf("Advertised Device found: %s RPA: %s\n", advertisedDevice->toString().c_str(),advertisedDevice->getAddress().isRpa() ? "Yes" : "No");
		client.publish("ble/scan",advertisedDevice->toString().c_str());
		if (advertisedDevice->haveName() && advertisedDevice->getName() == "PBL2-8813BF7276E8" && !advertisedDevice->getAddress().isRpa()) {
			//std::string add = "35:3f:0b:8a:bb:05";
			//std::string add1 = "6c:7e:67:c6:9e:3f";
			//if (advertisedDevice->getAddress().equals(NimBLEAddress(add,BLE_ADDR_RANDOM))) {
			Serial.printf("Found Our Service\n");
			/** stop scan before connecting */
			NimBLEDevice::getScan()->stop();
			/** Save the device reference in a global for the client to use*/
			advDevice = advertisedDevice;
			/** Ready to connect now */
			doConnect = true;
		}
	}
    /** Callback to process the results of the completed scan or restart it */
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        Serial.printf("Scan Ended, reason: %d, device count: %d; Restarting scan\n", reason, results.getCount());
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }
} scanCallbacks;

void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
    std::string str  = (isNotify == true) ? "Notification" : "Indication";
    str             += " from ";
    str             += pRemoteCharacteristic->getClient()->getPeerAddress().toString();
    str             += ": Service = " + pRemoteCharacteristic->getRemoteService()->getUUID().toString();
    str             += ", Characteristic = " + pRemoteCharacteristic->getUUID().toString();
    str             += ", Value = " + std::string((char*)pData, length);
    Serial.printf("%s\n", str.c_str());
	auto valuestr = std::string((char*)pData, length);
	std::string data;
	data.assign(valuestr.begin()+8, valuestr.end());
    Serial.printf("New %s\n", data.c_str());

	std::vector<uint8_t> z;
	z = hexToAscii(data);
	char txtbuf[256];

	unsigned long epochTime = getTime();


	if (data[3] == 'B') {
        ESP_LOGD("ble_client_lambda","Status Packet B");
		/*
		  id(burner).publish_state(z[35] == 1);
		  id(auger).publish_state(z[36] == 1);
		  id(power).publish_state(z[24] == 1);
		  id(nopellets).publish_state(z[32] == 1);
		  id(probeTemp).publish_state(calculateTemp(z,5));
		  id(grillTemp).publish_state(calculateTemp(z,17));
		*/
		sprintf(txtbuf,"{\"Timestamp\": %u, \"Igniter\": %i, \"Auger\": %i, \"Nopellets\": %i, \"Power\": %i, \"RSSI\": %d } \n", epochTime, z[35] == 1, z[36] == 1,z[32] == 1,z[24] == 1, pRemoteCharacteristic->getClient()->getRssi());
		client.publish("homeassistant/sensor/smoker01/status",txtbuf);
		Serial.printf("JSON: %s\n",txtbuf);	  

	}
	if (data[3] == 'C') {
		Serial.println("Status Packet C");
		Serial.printf("Grillset %i\n",calculateTemp(z,20));
		Serial.printf("GrillTemp %i\n",calculateTemp(z,23));
		Serial.printf("ProbeTemp %i\n",calculateTemp(z,5));
		
		sprintf(txtbuf,"{\"Timestamp\": %u, \"Setpoint\": %i, \"CurrentTemp\": %i, \"ProbeTemp\": %i}\n",epochTime, calculateTemp(z,20),calculateTemp(z,23),calculateTemp(z,5));
		client.publish("homeassistant/sensor/smoker01/temps",txtbuf);
		Serial.printf("JSON: %s\n",txtbuf);	  
	}
	/*
	  sprintf(txtbuf,"%i",setPoint);
	  client.publish("homeassistant/sensor/smoker01/setpoint",txtbuf);
	  sprintf(txtbuf,"%i",meatTemp);
	  client.publish("homeassistant/sensor/smoker01/meattemp",txtbuf);
	  sprintf(txtbuf,"%i",currTemp);
	  client.publish("homeassistant/sensor/smoker01/currtemp",txtbuf);
	  if (heaterState == false) {
	  client.publish("homeassistant/sensor/smoker01/heater","OFF");
	  } else {
	  client.publish("homeassistant/sensor/smoker01/heater","ON");
	  }
	  if (heaterEnable == true) {
	  client.publish("homeassistant/sensor/smoker01/enable","TRUE");
	  } else {
	  client.publish("homeassistant/sensor/smoker01/enable","FALSE");
	  }
	  
	*/  
	  

	
}

/** Handles the provisioning of clients and connects / interfaces with the server */
bool connectToServer() {
    NimBLEClient* pClient = nullptr;

    /** Check if we have a client we should reuse first **/
    if (NimBLEDevice::getCreatedClientCount()) {
        /**
         *  Special case when we already know this device, we send false as the
         *  second argument in connect() to prevent refreshing the service database.
         *  This saves considerable time and power.
         */
        pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if (pClient) {
            if (!pClient->connect(advDevice, false)) {
                Serial.printf("Reconnect failed\n");
                return false;
            }
            Serial.printf("Reconnected client\n");
        } else {
            /**
             *  We don't already have a client that knows this device,
             *  check for a client that is disconnected that we can use.
             */
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }

    /** No client to reuse? Create a new one. */
    if (!pClient) {
        if (NimBLEDevice::getCreatedClientCount() >= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) {
            Serial.printf("Max clients reached - no more connections available\n");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        Serial.printf("New client created\n");

        pClient->setClientCallbacks(&clientCallbacks, false);
        /**
         *  Set initial connection parameters:
         *  These settings are safe for 3 clients to connect reliably, can go faster if you have less
         *  connections. Timeout should be a multiple of the interval, minimum is 100ms.
         *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 150 * 10ms = 1500ms timeout
         */
        pClient->setConnectionParams(12, 12, 0, 150);

        /** Set how long we are willing to wait for the connection to complete (milliseconds), default is 30000. */
        pClient->setConnectTimeout(5 * 1000);

        if (!pClient->connect(advDevice)) {
            /** Created a client but failed to connect, don't need to keep it as it has no data */
            NimBLEDevice::deleteClient(pClient);
            Serial.printf("Failed to connect, deleted client\n");
            return false;
        }
    }

    if (!pClient->isConnected()) {
        if (!pClient->connect(advDevice)) {
            Serial.printf("Failed to connect\n");
            return false;
        }
    }

    Serial.printf("Connected to: %s RSSI: %d\n", pClient->getPeerAddress().toString().c_str(), pClient->getRssi());

    /** Now we can read/write/subscribe the characteristics of the services we are interested in */

    pSvc = pClient->getService("5F6D4F53-5F44-4247-5F53-56435F49445F");
    if (pSvc) {
        pChr = pSvc->getCharacteristic("306D4F53-5F44-4247-5F6C-6F675F5F5F30");
    } else {
		Serial.printf("445F service not found.\n");
		return false;
	}

    writeSvc = pClient->getService("5F6D4F53-5F52-5043-5F53-56435F49445F");
    if (writeSvc) {
        writeChr = writeSvc->getCharacteristic("5F6D4F53-5F52-5043-5F64-6174615F5F5F");
    } else {
		Serial.printf("445F service not found.\n");
		return false;
	}


	
    if (pChr) {
        if (pChr->canRead()) {
            Serial.printf("%s Value: %s\n", pChr->getUUID().toString().c_str(), pChr->readValue().c_str());
        }
        if (pChr->canNotify()) {
            if (!pChr->subscribe(true, notifyCB)) {
				Serial.printf("Failed to subscribe to F530 service.\n");
                pClient->disconnect();
                return false;
            }
        }
    } else {
        Serial.printf("F530 service not found.\n");
		return false;
    }

    Serial.printf("Done with this device!\n");
    return true;
}


void mqttConnect() {
	client.setServer(mqtt_server, mqtt_port);
	client.connect("smokerproxy", mqtt_username, mqtt_pass);
	client.subscribe("smokerproxy/set");
	client.subscribe("smokerproxy/heaterctl");
	//client.setCallback(mqttcallback);
}

void initWiFi() {
	int i = 0;
	pinMode(4, OUTPUT);

	analogWrite(4, 50);
	long wifistart = 0;

	WiFi.mode(WIFI_STA);
	WiFi.setHostname("smokerproxy");
	WiFi.begin(ssid, password);
	Serial.printf("Connecting to WiFi .. SSID %s  PASS %s\n",ssid,password);
	wifistart = millis();
	while (WiFi.status() != WL_CONNECTED) {
		Serial.print(".");
		if (i) { 
			analogWrite(4, 0);
		} else {
			analogWrite(4, 10);
		}
		i = !i;
		delay(1000);
		if (millis() - wifistart > 8000) {
			Serial.print("Failed to connect to wifi.  Restarting.");
			ESP.restart();
		}
	}
	ArduinoOTA.setHostname("smokerproxy");
	analogWrite(4, 0);

  
	ArduinoOTA
		.onStart([]() {
			String type;
			if (ArduinoOTA.getCommand() == U_FLASH)
				type = "sketch";
			else // U_SPIFFS
				type = "filesystem";

			Serial.println("Start updating " + type);
		})
		.onEnd([]() { Serial.println("\nEnd"); })
		.onProgress([](unsigned int progress, unsigned int total) {
			char txtbuf[20];
			sprintf(txtbuf, "Progress: %u%%\r", (progress / (total / 100)));
		})
		.onError([](ota_error_t error) {
			Serial.printf("Error[%u]: ", error);
			Serial.println("End Failed");
		});

	ArduinoOTA.begin();

	server.begin();


  
}



void setup() {
	Serial.begin(115200);
	Serial.printf("Boot\n");

    NimBLEDevice::init("NimBLE-Client");

    NimBLEDevice::setPower(3); /** 3dbm */
    NimBLEScan* pScan = NimBLEDevice::getScan();

    /** Set the callbacks to call when scan events occur, no duplicates */
    pScan->setScanCallbacks(&scanCallbacks, false);

    /** Set scan interval (how often) and window (how long) in milliseconds */
    pScan->setInterval(100);
    pScan->setWindow(100);

 
  

	initWiFi();

	initSNTP();
	
	mqttConnect();

	//printLocalTime();
	
	pScan->setActiveScan(true);

    /** Start scanning for advertisers */
    pScan->start(scanTimeMs);
    Serial.printf("Scanning for peripherals\n");


}


void loop() {

	unsigned long currentMillis = millis();

	ArduinoOTA.handle();
	WiFiClient httpclient = server.available();
	client.loop();

	// Update system time every 15 minutes
	/*
	if (currentMillis - previousMillis >= 5000) {
		previousMillis = millis();
		Serial.println(getTime());
	}
	*/

	
	if (WiFi.status() != WL_CONNECTED) {
		Serial.printf("Lost wifi connection.  Restarting\n");
		ESP.restart();
	}

	if (!client.connected()) {
		Serial.printf("Lost MQTT connection.  Restarting\n");
		ESP.restart();
	}
	 
	  
	delay(10);

	if (doConnect) {
		doConnect = false;
		/** Found a device we want to connect to, do it now */
		if (connectToServer()) {
			Serial.printf("Success! we should now be getting notifications, scanning for more!\n");
			if (0 && writeChr->canWrite()) {
				Serial.println("Writing!");
				writeChr->writeValue("FE0501020202FF");
			};
		} else {
			Serial.printf("Failed to connect, starting scan\n");
		}
	}

	//NimBLEDevice::getScan()->start(scanTimeMs);

 
}
