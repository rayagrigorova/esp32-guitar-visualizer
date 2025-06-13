# -*- coding: utf-8 -*-
"""
visualiser.py – Audio-to-LED front-end for color music visualizer
 - Splits audio into 8 logarithmic frequency bands (60 Hz to ~22 kHz)
 - Applies automatic gain control and frame-to-frame smoothing
"""
import numpy as np, sounddevice as sd
import serial, struct, queue, threading, sys, time

#  Notes: FFT stands for Fast Fourier Transform.
#  When we run FFT in a computer, we don’t get one smooth answer — we get a list of numbers.
#  Each item in that list is called a bin.
#  Each bin represents a small range of frequencies

# Serial port configuration
PORT     = 'COM4' # Port for ESP32 
DEVICE   = 1      # Input audio device index (use sd.query_devices() to list devices)
RATE     = 44100  # Sampling rate in Hz
SAMPLES  = 1024   # Number of samples per audio frame
BANDS    = 8      # Number of frequency bands to send

# Define the boundaries for the 8 frequency bands, spaced logarithmically.
# This means lower frequencies (bass) are more detailed, because that's where most guitar energy lives.
EDGE_HZ = np.logspace(np.log10(60), np.log10(RATE/2), BANDS+1)

# Compute the center frequency of each FFT bin, based on sample size and rate.
FREQS   = np.fft.rfftfreq(SAMPLES, 1/RATE) 

# Determine which frequency bin belongs to which of the 8 bands
# This creates a lookup table: for each bin -> which band (0 to 7) it belongs to
# For example, if a bin’s frequency is 300 Hz, and band 2 covers 250–400 Hz,
# then this bin is assigned to band 2.
BIN2BAND = np.digitize(FREQS, EDGE_HZ) - 1 

# Smoothing and automatic gain control (AGC) parameters
LEVEL_SMOOTH = 0.6     # Smoothing factor. 1.0 = no smoothing, 0.0 = completely frozen. Used to reduce flicker.
AGC_DECAY    = 0.995   # Automatic Gain Control (AGC) decay. Makes the visualizer auto-adjust for loud vs soft signals
peak = 1.0             # Keeps track of the loudest sound seen recently (for scaling all values)
last_print = 0         # Used to throttle how often debug info is printed

# Open the serial port
ser = serial.Serial(PORT, 500000)

# Create a thread-safe queue for passing processed frames to the sender
q   = queue.Queue(maxsize=16)

def callback(indata, frames, _time, status):
    global peak, last_print
    if status:
        print(status, file=sys.stderr)

    # Convert stereo (if present) to mono and apply a windowing function
    # Take only the first channel (mono) and apply a Hamming window
    # This reduces distortion in the FFT caused by abrupt edges in the sample
    mono = indata[:, 0] * np.hamming(frames)

    # Compute FFT (Fast Fourier Transform) to convert sound into frequency spectrum
    spec = np.abs(np.fft.rfft(mono))

    # Convert FFT into 8 frequency band levels
    bands = np.zeros(BANDS) # Create an empty array of 8 values
    for b in range(BANDS):
        idx = BIN2BAND == b # Find which FFT bins belong to this band
        if idx.any():
            bands[b] = np.sqrt(np.mean(spec[idx]**2)) # Compute root mean square energy for band
    
    # Smooth the band values compared to the previous frame
    # This reduces flickering by blending the new values with the old ones
    if 'prev' not in callback.__dict__:
        callback.prev = bands # If first run, just store this frame
    bands = LEVEL_SMOOTH * bands + (1-LEVEL_SMOOTH) * callback.prev
    callback.prev = bands # Save the current smoothed values for next time

    # Apply automatic gain control (AGC) to normalize the values
    # so loud and soft sounds both use the full LED brightness range (0–255)
    peak = max(peak*AGC_DECAY, bands.max())
    levels = np.clip((bands/peak)*255, 0, 255).astype(np.uint8)

    # Send the levels to the ESP32 over serial
    # Format: 1 start byte (0xFF) + 8 bytes of band levels
    try:
        q.put_nowait(b'\xFF' + struct.pack('<8B', *levels))
    except queue.Full:
        pass # Drop frame if queue is full

    # Print levels periodically for debugging
    if time.time() - last_print > 1.5:
        print('Levels:', list(levels)) # Example output: Levels: [124, 90, 40, 10, 2, 0, 0, 0]
        last_print = time.time()

def sender():
    """
    This function runs in a background thread and continuously sends
    prepared audio frames from the queue to the ESP32.
    """
    while True:
        ser.write(q.get()) # Take the next packet from the queue and send it

# Start a background thread to send serial data
threading.Thread(target=sender, daemon=True).start()

# Print audio device list and status
print('-> Devices:\n', sd.query_devices())
print('-> Listening …  Ctrl-C to stop')

# Start the audio stream and run until interrupted
with sd.InputStream(device=DEVICE, samplerate=RATE,
                    channels=1, blocksize=SAMPLES,
                    dtype='float32', latency='high',
                    callback=callback):
    try:  sd.sleep(10**9) # Keep the stream running for a long time
    except KeyboardInterrupt:  print('\nStopped')
