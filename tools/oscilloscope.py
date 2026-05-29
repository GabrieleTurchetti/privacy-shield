import sys
import serial
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

# Port configuration
PORT = 'COM4'
BAUD_RATE = 2000000

# Serial Connections
try:
    ser = serial.Serial(PORT, BAUD_RATE)
    # Clear old data stuck in the buffer
    ser.reset_input_buffer()
except Exception as e:
    print(f"Error! Port {PORT} is busy or not found.")
    print("Make sure you closed the Serial Monitor in VS Code!")
    sys.exit()

print("Listening...")

# High-Performance GUI Configuration
app = QtWidgets.QApplication([])
win = pg.GraphicsLayoutWidget(show=True, title="I2S Oscilloscope (60 FPS)")
win.resize(1000, 600)

plot = win.addPlot(title="Real-Time Audio Waveform (16kHz)")
# Set Y-axis limits (adjust if the wave is too small or too large)
plot.setYRange(-10000, 10000)

# Draw a glowing green line (Matrix/Oscilloscope style)
curve = plot.plot(pen=pg.mkPen('g', width=1)) 

# Data Buffer
window_size = 4000
data = np.zeros(window_size)
remainder = b'' # Stores data fragments between reads

# The update engine
def update():
    global data, remainder
    
    # If there is data waiting in the USB buffer
    if ser.in_waiting > 0:
        # Read the entire block at once (Bulk Read)
        raw_data = ser.read(ser.in_waiting)
        
        # Combine the remainder of the previous read with the new data
        full_data = remainder + raw_data
        
        # Split the data using the newline character
        lines = full_data.split(b'\n')
        
        # The last element is incomplete or empty, keep it for the next cycle
        remainder = lines.pop()
        
        new_values = []

        for line in lines:
            text = line.decode('utf-8', errors='ignore').strip()
            
            # If the line is empty, ignore it
            if not text:
                continue

            try:
                # Try the conversion
                val = int(text)
                new_values.append(val)
            except ValueError:
                pass
        
        # If we found valid numbers, update the graph
        if new_values:
            n = len(new_values)

            # Prevent overflow if too much data arrives at once
            if n > window_size:
                new_values = new_values[-window_size:]
                n = window_size
            
            # Shift the array to the left and insert the new data on the right
            data[:-n] = data[n:]
            data[-n:] = new_values
            
            # Send the array to the GPU for visual update
            curve.setData(data)

# 60 FPS Timer (1000 ms / 60 = ~16 ms)
# Ensures absolute smoothness without blocking the CPU
timer = QtCore.QTimer()
timer.timeout.connect(update)
timer.start(16)

# Start the application
if __name__ == '__main__':
    try:
        # Correct method for PyQt6
        app.exec()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\nOscilloscope closed safely.")