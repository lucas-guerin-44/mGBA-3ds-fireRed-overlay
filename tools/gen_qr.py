#!/usr/bin/env python3
"""Generate QR codes for mGBA-3DS reward payloads.

Usage:
  python gen_qr.py item <id> <qty> [<id> <qty> ...]   Give items
  python gen_qr.py heal                                 Heal party
  python gen_qr.py money <amount>                       Give money
  python gen_qr.py xp <amount> [slot]                   Give XP (slot 0-5, default 0)

Examples:
  python gen_qr.py item 1 1            # 1 Master Ball
  python gen_qr.py item 68 5           # 5 Rare Candies
  python gen_qr.py item 68 3 1 1       # 3 Rare Candies + 1 Master Ball
  python gen_qr.py heal                # Full heal
  python gen_qr.py money 10000         # +$10,000
  python gen_qr.py xp 5000            # +5000 XP to lead Pokemon
  python gen_qr.py xp 5000 2          # +5000 XP to slot 2

Requires: pip install qrcode[pil]
"""
import sys

def build_items(args):
    if len(args) % 2 != 0:
        print("Error: items need pairs of <id> <qty>")
        sys.exit(1)
    parts = []
    for i in range(0, len(args), 2):
        parts.append(f"{int(args[i])}x{int(args[i+1])}")
    payload = "PK1:ITEM:" + ",".join(parts)
    return payload, f"{len(parts)} item(s)"

def build_heal():
    return "PK1:HEAL", "heal party"

def build_money(args):
    amount = int(args[0])
    return f"PK1:MONEY:{amount}", f"${amount:,}"

def build_xp(args):
    amount = int(args[0])
    slot = int(args[1]) if len(args) > 1 else 0
    return f"PK1:XP:{slot}:{amount}", f"+{amount:,} XP to slot {slot}"

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "item":
        payload, desc = build_items(sys.argv[2:])
    elif cmd == "heal":
        payload, desc = build_heal()
    elif cmd == "money":
        payload, desc = build_money(sys.argv[2:])
    elif cmd == "xp":
        payload, desc = build_xp(sys.argv[2:])
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)

    print(f"Payload ({desc}): {payload}")

    try:
        import qrcode
        img = qrcode.make(payload, error_correction=qrcode.constants.ERROR_CORRECT_L)
        fname = f"reward_{cmd}.png"
        img.save(fname)
        print(f"  QR saved to: {fname}")
    except ImportError:
        print("  Install qrcode for PNG output: pip install qrcode[pil]")

if __name__ == "__main__":
    main()
