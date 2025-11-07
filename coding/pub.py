import paho.mqtt.client as mqtt
import time

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "v1/devices/ESP32_001/telemetry"

client = mqtt.Client()
client.connect(MQTT_BROKER, MQTT_PORT, 60)
client.loop_start()

try:
    count = 1
    while True:
        message = "{\"id\":\"ESP32_001\",\"payload\":\"B1C84F6E64\",\"date\":\"2025-11-03\",\"time\":\"20:16:05\"}"
        print(f"Publishing: '{message}' to topic '{MQTT_TOPIC}'")
        client.publish(MQTT_TOPIC, message)
        count += 1
        time.sleep(3) # Send a message every 3 seconds
except KeyboardInterrupt:
    print("Publishing stopped")
    client.loop_stop()
    client.disconnect()