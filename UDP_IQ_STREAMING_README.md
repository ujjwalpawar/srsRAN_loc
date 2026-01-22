# UDP IQ Sample Streaming - Usage Guide

## Overview

The PHY layer now transmits IQ correlation samples and time-domain channel estimates via UDP in real-time, non-blocking the TA estimation process.

## Implementation Details

### What Gets Transmitted

**Per TA estimation event (PUCCH/SRS/DMRS):**
- **Correlation vector**: Time-domain correlation power (magnitude squared)
- **IQ samples**: Complex time-domain channel observations from all antenna slices
- **Raw symbol IQ**: Full symbol IQ for symbol 12 across all subcarriers (single RX port to keep UDP payload under limits)
- **Metadata**: IMEISV, C-RNTI, TA value, timestamp, subframe/slot index
- **SRS allocation**: OFDM symbol indices and subcarrier indices (per UE)
- **Signal type**: SRS or DMRS indicator

### Packet Structure

```c
struct iq_udp_packet_header {
  uint64_t timestamp;           // Nanosecond timestamp
  char imeisv[16];              // UE IMEISV identifier
  uint16_t c_rnti;              // C-RNTI value
  uint16_t ta_flags;            // Bit-mask flags (bit0=1 -> TA came from RAR)
  int32_t ta_value;             // Current TA value
  uint64_t ta_update_time;      // Time when TA was last updated
  uint16_t subframe_index;      // Subframe index within the radio frame (0..9)
  uint16_t slot_index;          // Slot index within the radio frame
  uint16_t nof_symbols;         // Number of OFDM symbols carrying SRS
  uint16_t nof_subcarriers;     // Number of subcarriers carrying SRS
  uint16_t nof_srs_sequence;    // Number of SRS sequence samples (complex)
  uint16_t raw_symbol_index;    // OFDM symbol index for raw full-symbol capture (0xFFFF if unused)
  uint16_t raw_nof_ports;       // Number of RX ports included in raw symbol capture
  uint32_t raw_nof_subcarriers; // Number of subcarriers per RX port in raw symbol capture
  uint32_t nof_correlation;     // Number of correlation samples
  uint32_t nof_iq_samples;      // Number of IQ samples (complex)
  uint32_t nof_slices;          // Number of antenna slices
  uint8_t  signal_type;         // 0=unknown, 1=SRS, 2=DMRS
};

struct iq_udp_packet {
  iq_udp_packet_header header;
  float correlation[nof_correlation];       // Real correlation values
  float iq_samples[2 * nof_iq_samples];      // Interleaved I/Q (complex samples)
  uint16_t srs_symbols[nof_symbols];         // OFDM symbol indices carrying SRS
  uint16_t srs_subcarriers[nof_subcarriers]; // Subcarrier indices carrying SRS
  float raw_symbol_iq[2 * raw_nof_ports * raw_nof_subcarriers]; // Raw full-symbol IQ (I/Q interleaved)
  float srs_sequence[2 * nof_srs_sequence];  // SRS sequence (I/Q interleaved)
};
```

Note: When raw symbol capture is enabled, the UDP payload includes only the first antenna slice in `iq_samples` to stay
under UDP size limits. The `nof_slices` field reflects this.

## Configuration

### Enable UDP Streaming

Set environment variables before running gnb:

```bash
export SRSRAN_IQ_UDP_IP="127.0.0.1"    # Receiver IP
export SRSRAN_IQ_UDP_PORT="12345"      # Receiver port
```

### Run gNB

```bash
cd /home/netsys/srsran_april2025/build/apps/gnb
sudo ./gnb -c <your_config.yml>
```

UDP sender initializes automatically if environment variables are set.

### Run Receiver

In another terminal:

```bash
cd /home/netsys/srsran_april2025
python3 udp_iq_receiver_dynamic.py --port 12345
```

## Features

### Non-Blocking Design

- **Asynchronous queue**: UDP sender runs in separate thread
- **Lock-free enqueue**: TA estimation never waits for network I/O
- **Drop on overflow**: If queue fills (>100 packets), new packets drop gracefully
- **Statistics**: Track queue size and dropped packet count

### Real-Time Monitoring

Python receiver displays:
- Packet count and timestamp
- IMEISV and C-RNTI
- TA value
- Correlation peak location
- IQ sample magnitude statistics

### Data Capture

Every 10th packet automatically saves to CSV:
- `correlation_<IMEISV>_<count>.csv` - Correlation power profile
- `iq_samples_<IMEISV>_<count>.csv` - Complex IQ time-domain samples

## Use Cases

### Real-Time TA Analysis
Monitor correlation peaks to validate TA estimation algorithm:
```python
# Plot correlation in real-time
import matplotlib.pyplot as plt
plt.plot(correlation)
plt.title(f"TA Correlation - C-RNTI {c_rnti}")
plt.show()
```

### Channel Impulse Response
Analyze time-domain channel from IQ samples:
```python
# Compute magnitude
magnitude = [abs(complex(i, q)) for i, q in zip(i_samples, q_samples)]
# Find multipath delays
peaks = find_peaks(magnitude, threshold=max(magnitude)*0.3)
```

### UE Tracking
Track multiple UEs simultaneously via IMEISV/C-RNTI:
```python
ue_database[imeisv] = {
    'c_rnti': c_rnti,
    'ta_history': [],
    'correlation_peaks': []
}
```

## Performance

- **Queue size**: 100 packets (~400KB memory per packet = 40MB queue)
- **Throughput**: ~10-100 packets/sec depending on UE activity
- **Latency**: <1ms enqueue time (non-blocking)
- **Network**: ~50KB per packet (correlation + IQ samples)

## Troubleshooting

### No packets received

Check:
1. Environment variables set before gnb start
2. Firewall allows UDP port
3. Receiver running before gnb starts

```bash
# Check if port is listening
sudo netstat -tulpn | grep 12345

# Test with netcat
nc -u -l 12345
```

### Dropped packets

```
[UDP_SENDER] Dropped packets: 1234
```

Solutions:
- Increase MAX_QUEUE_SIZE in header (currently 100)
- Reduce receiver processing time
- Use faster network interface

### Wrong data received

Verify struct alignment matches between C++ and Python:
- Header size is 71 bytes (packed)
- Total size = header + `4 * nof_correlation` + `4 * 2 * nof_iq_samples` + `2 * nof_symbols` + `2 * nof_subcarriers` +
  `4 * 2 * raw_nof_ports * raw_nof_subcarriers` + `4 * 2 * nof_srs_sequence`

## Advanced Usage

### Multi-UE Visualization Dashboard

```python
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

fig, (ax1, ax2) = plt.subplots(2, 1)

def update(frame):
    # Receive packet
    data, addr = sock.recvfrom(65536)
    # Parse and plot
    ax1.clear()
    ax1.plot(correlation)
    ax1.set_title(f"Correlation - {imeisv}")
    
    ax2.clear()
    ax2.plot([abs(complex(i,q)) for i,q in zip(i_samples, q_samples)])
    ax2.set_title(f"Channel IR - TA={ta_value}")

ani = FuncAnimation(fig, update, interval=100)
plt.show()
```

### MATLAB Integration

Save to binary and load in MATLAB:
```python
with open('iq_dump.bin', 'wb') as f:
    f.write(struct.pack(f'<{len(i_samples)}f', *i_samples))
    f.write(struct.pack(f'<{len(q_samples)}f', *q_samples))
```

```matlab
fid = fopen('iq_dump.bin', 'r');
i_samples = fread(fid, nof_samples, 'float32');
q_samples = fread(fid, nof_samples, 'float32');
iq = i_samples + 1j*q_samples;
plot(abs(iq));
```

## Debugging

Enable verbose logging in C++:
```cpp
// In async_udp_sender::send_async()
std::cout << "[UDP_SENDER] Queue: " << queue_size_ 
          << " Dropped: " << dropped_packets_ << std::endl;
```

Packet capture:
```bash
sudo tcpdump -i lo udp port 12345 -w iq_capture.pcap
```

## Notes

- **File writes still happen**: UDP streaming is additional, file logging unchanged
- **Environment variables**: Only checked once at first TA estimation
- **Thread safety**: Queue protected by mutex, sender thread isolated
- **Memory**: Large packet size (~50KB), consider memory if queue grows

## Contact

For issues or questions about UDP streaming implementation, check:
- `lib/phy/support/time_alignment_estimator/time_alignment_estimator_dft_impl.cpp`
- `lib/phy/support/time_alignment_estimator/time_alignment_estimator_dft_impl.h`
