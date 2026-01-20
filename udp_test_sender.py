#!/usr/bin/env python3
import socket
import struct
import time
import numpy as np

# Use same header format
HEADER_FORMAT = '<Q16sHHIQHHHHHHHIIII'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

UDP_IP = '127.0.0.1'
UDP_PORT = 12345

# Fill header values
timestamp = int(time.time_ns())
imeisv = '0123456789012345'  # 16 bytes
c_rnti = 0x1234
ta_flags = 1  # bit0 -> RAR TA
ta_value = 5
ta_update_time = int(time.time_ns())
subframe_index = 7
slot_index = 14
nof_correlation = 4
nof_iq_samples = 8  # complex samples count
nof_slices = 2
nof_srs_sequence = 4
raw_symbol_index = 12
raw_nof_ports = 2
raw_nof_subcarriers = 4

# Create correlation floats
correlation = np.linspace(0, 1, num=nof_correlation, dtype=np.float32)
# SRS symbol/subcarrier lists
srs_symbols = np.array([3, 4], dtype=np.uint16)
srs_subcarriers = np.array([100, 104, 108, 112], dtype=np.uint16)
nof_symbols = len(srs_symbols)
nof_subcarriers = len(srs_subcarriers)
# Create interleaved IQ: nof_iq_samples complex values -> 2*nof_iq_samples floats
iq_raw = np.zeros(nof_iq_samples * 2, dtype=np.float32)
for i in range(nof_iq_samples):
    iq_raw[2*i] = float(i)       # I
    iq_raw[2*i+1] = float(i) * -1.0  # Q

# Pack header
header = struct.pack(HEADER_FORMAT,
                     timestamp,
                     imeisv.encode('utf-8'),
                     c_rnti,
                     ta_flags,
                     ta_value,
                     ta_update_time,
                     subframe_index,
                     slot_index,
                     nof_symbols,
                     nof_subcarriers,
                     nof_srs_sequence,
                     raw_symbol_index,
                     raw_nof_ports,
                     raw_nof_subcarriers,
                     nof_correlation,
                     nof_iq_samples,
                     nof_slices)
# Pack correlation
corr_bytes = struct.pack(f'<{nof_correlation}f', *correlation)
# Pack IQ
iq_bytes = struct.pack(f'<{nof_iq_samples*2}f', *iq_raw)
# Pack SRS symbol/subcarrier lists
symbols_bytes = struct.pack(f'<{nof_symbols}H', *srs_symbols)
subcarriers_bytes = struct.pack(f'<{nof_subcarriers}H', *srs_subcarriers)

raw_iq_samples = raw_nof_ports * raw_nof_subcarriers
raw_iq_raw = np.zeros(raw_iq_samples * 2, dtype=np.float32)
for i in range(raw_iq_samples):
    raw_iq_raw[2*i] = float(i + 100)      # I
    raw_iq_raw[2*i+1] = float(i + 100) * -1.0  # Q
raw_iq_bytes = struct.pack(f'<{raw_iq_samples*2}f', *raw_iq_raw)

srs_seq_raw = np.zeros(nof_srs_sequence * 2, dtype=np.float32)
for i in range(nof_srs_sequence):
    srs_seq_raw[2*i] = float(i + 1)      # I
    srs_seq_raw[2*i+1] = float(i + 1) * 0.5  # Q
srs_seq_bytes = struct.pack(f'<{nof_srs_sequence*2}f', *srs_seq_raw)

packet = header + corr_bytes + iq_bytes + symbols_bytes + subcarriers_bytes + raw_iq_bytes + srs_seq_bytes

# Send packet
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(packet, (UDP_IP, UDP_PORT))
print('Packet sent:', len(packet), 'bytes')

sock.close()
