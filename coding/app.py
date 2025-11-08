import paho.mqtt.client as mqtt
from flask import Flask, render_template, make_response
import csv
import io
from flask_socketio import SocketIO
import traceback
import json
from collections import deque
from datetime import datetime
from typing import Optional, Dict, Any
import threading

# --- Flask and SocketIO Initialization ---
app = Flask(__name__)
# A secret key is required for SocketIO to work securely
app.config['SECRET_KEY'] = 'your_secret_key!'  
socketio = SocketIO(app)

# --- MQTT Configuration ---
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
# This is the topic your server will subscribe to
MQTT_TOPIC = "v1/devices/ESP32_001/telemetry"  
output_message = ""

# --- In-Memory Data Storage ---
# We will store the GPS data here.
# We need a lock to make this thread-safe!
data_log = []
data_lock = threading.Lock()

# ----------------- Utility: payload decode -----------------
def u16_to_lon(u16: int) -> float:
    """Map uint16 (0..65535) to longitude (-180..180)."""
    return (u16 * 360.0 / 65535.0) - 180.0

def u16_to_lat(u16: int) -> float:
    """Map uint16 (0..65535) to latitude (-90..90)."""
    return (u16 * 180.0 / 65535.0) - 90.0

def decode_payload_hex(payload_hex: str) -> Dict[str, Any]:
    """
    Decode payload hex structured as:
      - first 4 hex digits -> longitude (u16)
      - next 4 hex digits  -> latitude (u16)
      - last 2 hex digits  -> battery percent (0..100)
    Returns dict with 'lon', 'lat', 'battery', and any raw fields.
    """
    info = {"raw": payload_hex}
    try:
        # Normalize: remove 0x prefix and uppercase
        ph = payload_hex.strip()
        if ph.startswith("0x") or ph.startswith("0X"):
            ph = ph[2:]
        ph = ph.upper()

        # Expect at least 10 hex chars (4+4+2). If shorter, attempt to pad or error.
        if len(ph) < 10:
            # Pad right with zeros if short (best-effort), but mark it
            ph = ph.ljust(10, "0")

        lon16 = int(ph[0:4], 16)
        lat16 = int(ph[4:8], 16)
        batt8 = int(ph[8:10], 16)

        lon = u16_to_lon(lon16)
        lat = u16_to_lat(lat16)
        batt = max(0, min(100, batt8))  # clamp 0..100

        info.update({"lon": lon, "lat": lat, "battery": batt, "lon16": lon16, "lat16": lat16, "batt8": batt8})
    except Exception as e:
        info.update({"error": str(e)})
    return info

# --- MQTT Client Setup ---
def on_connect(client, userdata, flags, rc):
    """ The callback for when the client receives a CONNACK response from the server. """
    if rc == 0:
        print("Connected to MQTT Broker!")
        # Subscribe to the topic upon connection
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"Failed to connect, return code {rc}\n")

def on_message(client, userdata, msg):
    """ 
    The callback for when a PUBLISH message is received from the server.
    This is where we forward the message to the web clients.
    """
    print(f"Received message '{msg.payload.decode()}' on topic '{msg.topic}'")
def on_message(client, userdata, msg):
    global output_message
    try:
        s = msg.payload.decode("utf-8")
        print("receive message", s)
        # parse JSON telemetry
        data = json.loads(s)
        # Expected keys: id, payload, date, time
        device_id = data.get("id", "unknown")
        payload_hex = data.get("payload", "")
        date_s = data.get("date", "")
        time_s = data.get("time", "")
        rx_ts = datetime.utcnow().isoformat()

        decoded = decode_payload_hex(payload_hex)
        entry = {
            "device_id": device_id,
            "payload_hex": payload_hex,
            "date": date_s,
            "time": time_s,
            "rx_timestamp": rx_ts,
            "topic": msg.topic,
            "decoded": decoded
        }
        output_message = entry
        print("receive message decoded", entry)
        # Emit the message to all connected web clients
        # We use a custom event name 'mqtt_message'
        # Here is not working, need to do in seperate thread
        # socketio.emit('mqtt_message', {'topic': msg.topic, 'payload': msg.payload.decode()})

        # Save the data to log
        with data_lock:
            data_log.append({
                'timestamp': rx_ts,
                'lat': decoded['lat'],
                'lon': decoded['lon']
            })

    except Exception as e:
        print("Error parsing MQTT message: %s\n%s" % (e, traceback.format_exc()))



# Initialize MQTT client
client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

# Connect to the broker
client.connect(MQTT_BROKER, MQTT_PORT, 60)

# Start the MQTT client loop in a non-blocking way
# This loop handles reconnecting and processing messages
client.loop_start()

# --- Flask Routes ---
@app.route('/')
def index():
    """Serve the index page."""
    return render_template('index.html')

@app.route('/download')
def download_csv():
    """
    This new route generates and sends the CSV file.
    """
    print("Download request received. Generating CSV...")
    
    # Create an in-memory string buffer
    si = io.StringIO()
    
    # Create a CSV writer
    writer = csv.writer(si)
    
    # Write the CSV header
    writer.writerow(['timestamp', 'latitude', 'longitude'])
    
    # --- Thread-safe read from our log ---
    # We copy the data inside the lock to minimize blocking
    log_copy = []
    with data_lock:
        log_copy = data_log.copy()
    # --- End of thread-safe block ---
        
    # Write the data rows
    if log_copy:
        for row in log_copy:
            writer.writerow([row['timestamp'], row['lat'], row['lon']])
    
    # Get the CSV data as a string
    output = si.getvalue()
    
    # Create a Flask response object
    response = make_response(output)
    
    # Set the headers to trigger a file download
    response.headers["Content-Disposition"] = "attachment; filename=gps_data.csv"
    response.headers["Content-Type"] = "text/csv"
    
    print("Sending CSV file to client.")
    return response

# --- SocketIO Events ---
@socketio.on('connect')
def handle_connect():
    """Event handler for new client connections."""
    print('Web client connected')

@socketio.on('disconnect')
def handle_disconnect():
    """Event handler for client disconnections."""
    print('Web client disconnected')

def background_emitter():
    """
    Emit in the on_message is not working somehow.
    We need to create the thread to emit instead
    """
    global output_message
    print("Starting background test emitter...")
    count = 0
    while True:
        count += 1
        message = f"This is background test message #{count}"
        
        # Print to server console
        print(f"[BG TASK] Emitting: '{message}'")
        
        if output_message:
            socketio.emit('mqtt_message', {'topic': MQTT_TOPIC,
            "device_id": output_message['device_id'],
            "date": output_message['date'],
            "time": output_message['time'],
            "lat": output_message['decoded']['lat'],
            "lon": output_message['decoded']['lon'],
            "battery": output_message['decoded']['battery']})
            output_message = ""
        
        # Use socketio.sleep() for async-friendly sleeping
        socketio.sleep(1)

# --- Run the Application ---
if __name__ == '__main__':
    socketio.start_background_task(background_emitter)

    # Run the Flask-SocketIO server
    # allow_unsafe_werkzeug=True is needed for newer SocketIO versions with Flask
    socketio.run(app, host='127.0.0.1', port=5000)