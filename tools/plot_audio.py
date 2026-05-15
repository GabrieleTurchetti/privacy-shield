import serial
import wave
import struct
import sys

# Serial configuration
PORT = 'COM4' # Replace with your exact port
BAUD_RATE = 2000000
SAMPLE_RATE = 16000 
OUTPUT_FILENAME = "audio.wav"

try:
    ser = serial.Serial(PORT, BAUD_RATE)
    ser.reset_input_buffer() 
except Exception as e:
    print(f"Error! Port {PORT} is busy or not found.")
    sys.exit()

print("Listening... Speak into the microphone!")
print("Press Ctrl+C to stop and save the .wav file.")
recorded_samples = [] 

try:
    while True:
        # Read and decode the serial stream
        text_line = ser.readline().decode('utf-8').strip()
        
        try:
            raw_value = int(text_line) 
            recorded_samples.append(raw_value)            
                
        except ValueError:
            # Ignore invalid string conversions
            pass 

except KeyboardInterrupt:
    print(f"\nClosing and processing {len(recorded_samples)} samples...")
    ser.close()
    
    # WAV export logic
    print(f"Saving audio to '{OUTPUT_FILENAME}'...")
    
    with wave.open(OUTPUT_FILENAME, 'w') as wav_file:
        wav_file.setnchannels(1)   # Mono
        wav_file.setsampwidth(2)   # 2 bytes per sample
        wav_file.setframerate(SAMPLE_RATE)
        
        for sample in recorded_samples:
            # 16-bit WAV requires values strictly between -32768 and 32767
            clamped_sample = int(max(-32768, min(32767, sample)))
            
            # Pack as little-endian short
            byte_data = struct.pack('<h', clamped_sample)
            wav_file.writeframesraw(byte_data)
            
    print("Done! Go listen to your file.")