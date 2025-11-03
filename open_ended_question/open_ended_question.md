# Open ended Question
## 1. Compare MQTT and HTTP for IoT data transmission. Which would you choose for batterypowered GPS trackers? Why?
  I would choose the MQTT. The device is the low resource device, so we need the lightweight communication model.
  We need to minimize the power consumption and bandwidth for the communication.
  Reasons
  1. Communication model
  - For the periodic data, Pub/sub(MQTT) model is better that the req/resp(HTPP). 
  - For req/resp, we need an overhead time to initiate the communication and wait for resp. 
  - For pub/sub, publisher and subscriber can work independently. No blocking.
  - Easy to do one to many. 1 publisher can have many subscribers. 1 subscriber can have many data from publishers.
  2. Network Efficiency
  - MQTT have smaller overhead. The header of message require only 2 bytes.
  ![mqtt message structure](mqtt_structure.png) ref. http://www.steves-internet-guide.com/mqtt-protocol-messages-overview/
  - HTTP require a lot of header
  ![http message structure](http_structure.png) ref. https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/Messages
  - Lower data, so we can have a minimal bandwidth
  3. Power Consumption
  - MQTT is minimal. They have lower operations needed for the communication, so the lower power consumption than HTTP.
  4. Community
  - They have a lot of approch using MQTT for the IoT device.
## 2. How would you reconnect an ESP32 automatically when Wi-Fi connection drops?
  Basically, If we are using MQTT and Arduino, the library should handle the reconnection automatically.
  But, if it is not working, we should have the non-blocking process for the reconnection.
  In the disconnected state, we may have the crucial information that we don't want to lost it.
  Hence we should save this information in the limited memory and we can upload this data when the connection is back.
  In the same time, the program should retry the conection.

  From the https://github.com/espressif/arduino-esp32/blob/fc8ce8f80800bc271019c67d00ba566ce2bb309f/libraries/WiFi/src/STA.cpp#L58, the WIFI library will reconnect if the status falls to reconnectable reasons. We can set the wifi event to check for that. If it is not a reconnectable reasons, we can set the flag to reconnection in the main loop.
  To retry the connection, we should find the suitable period. Not too much, we lost the power. Not to slow, we don't want to disconnect for a long time.
  After WIFI is reconnect, we should reconnect the communication(MQTT) as well.
  
## 3. Describe how you would implement over-the-air (OTA) firmware updates securely for ESP32 IoT devices.
## 4. How would you reduce ESP32 power consumption during idle periods?
## 5. How would you filter GPS noise or jitter in position readings?