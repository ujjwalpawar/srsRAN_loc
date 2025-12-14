#!/usr/bin/env python3
import socket
import struct
import numpy as np
import csv
import os
import argparse
from datetime import datetime

# Parse CLI args
parser = argparse.ArgumentParser(description='UDP IQ Sample Receiver (Dynamic Size)')
parser.add_argument('--ip', type=str, default='0.0.0.0', help='IP to listen on')
parser.add_argument('--port', type=int, default=12345, help='UDP port to listen on')
parser.add_argument('--save-iq', action='store_true', default=False, help='Save IQ samples per timestamp to .npz files')
parser.add_argument('--outdir', type=str, default='.', help='Output directory for saving IQ files')
args = parser.parse_args()

UDP_IP = args.ip
UDP_PORT = args.port
SAVE_IQ = args.save_iq
# Save per antenna port by default
SAVE_PER_PORT = True
OUTDIR = args.outdir

# Header structure (52 bytes)
HEADER_FORMAT = '<Q16sHHIQIII'  # timestamp, imeisv[16], c_rnti, ta_flags, ta_value, ta_update_time, nof_correlation, nof_iq_samples, nof_slices
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
TA_FLAG_RAR = 0x1

print(f"UDP IQ Sample Receiver (Dynamic Size)")
print(f"Listening on {UDP_IP}:{UDP_PORT}")
print(f"Header size: {HEADER_SIZE} bytes")
print(f"Save IQ -> {SAVE_IQ}  | Output dir -> {OUTDIR} | Per-port save -> {SAVE_PER_PORT}")
print(f"Ready to receive variable-sized packets...")
print()

# Create UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))

# CSV file setup
csv_filename = f"iq_samples_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
csv_file = None
csv_writer = None
packet_count = 0

try:
    while True:
        # Receive packet
        data, addr = sock.recvfrom(1024 * 1024)  # 1MB buffer for large packets
        
        if len(data) < HEADER_SIZE:
            print(f"Received packet too small: {len(data)} bytes")
            continue
        
        # Parse header
        header_data = data[:HEADER_SIZE]
        timestamp, imeisv_bytes, c_rnti, ta_flags, ta_value, ta_update_time, nof_correlation, nof_iq_samples, nof_slices = struct.unpack(HEADER_FORMAT, header_data)
        
        # Decode IMEISV
        imeisv = imeisv_bytes.decode('utf-8').rstrip('\x00')
        
        # Calculate expected data size
        correlation_size = nof_correlation * 4  # floats
        iq_samples_size = nof_iq_samples * 2 * 4  # complex floats (I/Q pairs)
        expected_size = HEADER_SIZE + correlation_size + iq_samples_size
        
        if len(data) != expected_size:
            print(f"⚠ Size mismatch: received {len(data)}, expected {expected_size}")
            print(f"  Header: {HEADER_SIZE}, Correlation: {correlation_size}, IQ: {iq_samples_size}")
            continue
        
        # Parse correlation array
        corr_offset = HEADER_SIZE
        correlation = struct.unpack(f'<{nof_correlation}f', data[corr_offset:corr_offset + correlation_size])
        
        # Parse IQ samples (interleaved I/Q)
        iq_offset = corr_offset + correlation_size
        iq_raw = struct.unpack(f'<{nof_iq_samples * 2}f', data[iq_offset:iq_offset + iq_samples_size])
        
        # Separate I and Q
        i_samples = np.array(iq_raw[::2])
        q_samples = np.array(iq_raw[1::2])
        
        packet_count += 1
        
        print(f"\n{'='*70}")
        print(f"Packet #{packet_count} from {addr}")
        print(f"{'='*70}")
        print(f"Timestamp: {timestamp}")
        print(f"IMEISV: {imeisv}")
        print(f"C-RNTI: {c_rnti}")
        print(f"TA Value: {ta_value}")
        print(f"TA Update Time: {ta_update_time}")
        print(f"TA Flags: 0x{ta_flags:04x} (RAR={'yes' if (ta_flags & TA_FLAG_RAR) else 'no'})")
        print(f"Correlation samples: {nof_correlation}")
        print(f"Total IQ samples: {nof_iq_samples}")
        print(f"Number of antenna slices: {nof_slices}")
        print(f"Packet size: {len(data)} bytes")
        print()
        
        # Rearrange by antenna port
        if nof_slices > 0 and nof_iq_samples > 0:
            samples_per_slice = nof_iq_samples // nof_slices
            print(f"Shape before split: {len(i_samples)} samples total")
            print(f"Samples per antenna: {samples_per_slice}")
            print()
            
            for port_idx in range(nof_slices):
                start = port_idx * samples_per_slice
                end = start + samples_per_slice
                port_i = i_samples[start:end]
                port_q = q_samples[start:end]
                
                print(f"Antenna Port {port_idx}: {len(port_i)} symbols")
                if len(port_i) > 0:
                    print(f"  First 3: ", end="")
                    for j in range(min(3, len(port_i))):
                        print(f"({port_i[j]:.6f}, {port_q[j]:.6f}) ", end="")
                    print()
            # Optionally save IQ data into a file named by the timestamp
            if SAVE_IQ:
                try:
                    os.makedirs(OUTDIR, exist_ok=True)
                    outname = os.path.join(OUTDIR, f"iq_{timestamp}.npz")
                    if os.path.exists(outname):
                        # avoid overwriting existing file - append a counter if necessary
                        base = outname.rstrip('.npz')
                        idx = 1
                        while os.path.exists(f"{base}-{idx}.npz"):
                            idx += 1
                        outname = f"{base}-{idx}.npz"

                    # Reconstruct interleaved complex array
                    iq_complex = (i_samples.astype(np.float32) + 1j * q_samples.astype(np.float32)).astype(np.complex64)

                    # Base payload (metadata + correlation)
                    save_kwargs = dict(
                        correlation=np.array(correlation, dtype=np.float32),
                        imeisv=np.array([imeisv], dtype='S16'),
                        c_rnti=np.array([c_rnti], dtype=np.uint16),
                        ta_value=np.array([ta_value], dtype=np.int32),
                        ta_update_time=np.array([ta_update_time], dtype=np.uint64),
                        ta_flags=np.array([ta_flags], dtype=np.uint16),
                        nof_slices=np.array([nof_slices], dtype=np.uint32),
                    )

                    # Optionally save per-port 2D arrays
                    # If per-port arrays are enabled, try to create 2D arrays and save only them
                    per_port_ok = False
                    if SAVE_PER_PORT and nof_slices > 0 and nof_iq_samples > 0:
                        if nof_iq_samples % nof_slices == 0:
                            samples_per_slice = nof_iq_samples // nof_slices
                            try:
                                i_by_port = i_samples.reshape(nof_slices, samples_per_slice)
                                q_by_port = q_samples.reshape(nof_slices, samples_per_slice)
                                iq_by_port = iq_complex.reshape(nof_slices, samples_per_slice)
                                save_kwargs['i_by_port'] = i_by_port.astype(np.float32)
                                save_kwargs['q_by_port'] = q_by_port.astype(np.float32)
                                save_kwargs['iq_by_port'] = iq_by_port.astype(np.complex64)
                                per_port_ok = True
                            except Exception as e:
                                print(f"Error reshaping into per-port arrays: {e}")
                        else:
                            print(f"Warning: cannot reshape IQ samples into equal per-port arrays: nof_iq_samples={nof_iq_samples}, nof_slices={nof_slices}")

                    # If per-port arrays are not saved, include flat arrays
                    if not per_port_ok:
                        save_kwargs['iq'] = iq_complex
                        save_kwargs['i'] = i_samples.astype(np.float32)
                        save_kwargs['q'] = q_samples.astype(np.float32)

                    # Save everything to NPZ using a temporary file + atomic rename for safety
                    tmp_outname = outname + '.tmp.npz'
                    try:
                        np.savez_compressed(tmp_outname, **save_kwargs)
                        os.replace(tmp_outname, outname)
                    except Exception as e:
                        if os.path.exists(tmp_outname):
                            try:
                                os.remove(tmp_outname)
                            except Exception:
                                pass
                        raise
                    print(f"Saved IQ+corr to {outname}")
                except Exception as e:
                    print(f"Error saving IQ file: {e}")

except KeyboardInterrupt:
    print("\n\nShutting down...")
finally:
    if csv_file:
        csv_file.close()
    sock.close()
    print(f"Total packets received: {packet_count}")
