import subprocess
import yaml
import logging

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# Load configuration
with open('config.yaml', 'r') as config_file:
    config = yaml.safe_load(config_file)

# Protocol: 304.30 MHz OOK, 333 us per chip, '10b' bit encoding.
# Message = preamble(4) + FAN_ID(14) + cmdid(7) = 25 bits = 75 chips.
# See protocol.txt for full details.
SENDOOK = 'sendook -f 304300000 -0 333 -1 333 '
PAUSE = '0' * 30

# 7-bit command codes. Same table for every fan — no per-fan modifications,
# no channel marker bits, no families. Just look up the function.
# Bit positions (left to right, MSB first):
#   bit 0 = speed6, bit 1 = speed3, bit 2 = speed1,
#   bit 3 = reverse,
#   bit 4 = stop,   bit 5 = light,  bit 6 = (unused, always 0).
# Three speeds and the pair button are OR-combinations of two single-bit codes.
CMDS = {
    'reverse': '0001000',
    'light':   '0000010',
    'stop':    '0000100',
    'speed1':  '0010000',
    'speed2':  '0010100',   # speed1 + stop
    'speed3':  '0100000',
    'speed4':  '0110000',   # speed3 + speed1
    'speed5':  '1000100',   # speed6 + stop
    'speed6':  '1000000',
    'pair':    '0100100',   # speed3 + stop — set-remote / pairing-mode trigger
}

# 14-bit FAN_IDs from config.yaml. Each fan has a unique address; no per-fan
# transform is applied to the cmdid before TX.
FAN_IDS = config['fans']


def construct_raw_bits_from_bits(bits):
    """Each logical data bit is encoded as 3 chips: '1', '0', b."""
    return ''.join(f'10{b}' for b in bits)


def construct_full_raw_cmd(fan_id, cmdid):
    preamble = '1111'
    logging.info(f"Sending bits: {preamble} | {fan_id} | {cmdid}")
    assert len(fan_id) == 14, f"FAN_ID must be 14 bits, got {len(fan_id)}"
    assert len(cmdid) == 7,  f"cmdid must be 7 bits, got {len(cmdid)}"
    return construct_raw_bits_from_bits(preamble + fan_id + cmdid) + PAUSE


def control_fan(room_name, cmd):
    logging.info(f"Control fan: {room_name}, {cmd}")
    if room_name not in FAN_IDS:
        raise ValueError(f"Invalid room name: {room_name}")
    if cmd not in CMDS:
        raise ValueError(f"Invalid command: {cmd}")

    full_cmd = SENDOOK + construct_full_raw_cmd(FAN_IDS[room_name], CMDS[cmd])
    logging.debug(f"Executing command: {full_cmd}")

    try:
        result = subprocess.run(full_cmd, shell=True, check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        logging.info(f"Command executed. Return code: {result.returncode}")
        if result.stdout:
            logging.debug(f"Command stdout: {result.stdout}")
        if result.stderr:
            logging.debug(f"Command stderr: {result.stderr}")
    except subprocess.CalledProcessError as e:
        logging.error(f"Error executing command. Return code: {e.returncode}")
        if e.stdout:
            logging.error(f"Command stdout: {e.stdout}")
        if e.stderr:
            logging.error(f"Command stderr: {e.stderr}")
    except Exception as e:
        logging.error(f"Unexpected error executing command: {str(e)}")


# Example usage:
# control_fan('living_room', 'light')
# control_fan('upstairs', 'stop')
