"""Copies a save file in Nexus DCI format into Voldorium's memory card image (vmu_a1.bin).

A .dci file is one VMU file: its 32-byte directory entry, then its blocks with every 32-bit
word byte-reversed. The card image (port/m1/maple.c) is a 128 KB VMU file system: FAT in block
254 (one 16-bit word per block: 0xfffc free, 0xfffa last, else the next block), directory in
blocks 253 down to 241 (16 entries of 32 bytes), user blocks 0-199. A file of the same name on
the card is replaced; the card is backed up to <card>.bak first.

usage: python -I tools/vmu_import.py <vmu_a1.bin> <save.dci>
"""
import os
import shutil
import struct
import sys

FREE, LAST = 0xFFFC, 0xFFFA
FAT_BLOCK, DIR_FIRST, DIR_LAST, USER_BLOCKS = 254, 253, 241, 200


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    card_path, dci_path = sys.argv[1], sys.argv[2]
    card = bytearray(open(card_path, 'rb').read())
    if len(card) != 128 * 1024 or card[255 * 512:255 * 512 + 16] != b'\x55' * 16:
        raise SystemExit('%s: not a formatted 128 KB memory card image' % card_path)
    dci = open(dci_path, 'rb').read()
    entry = bytearray(dci[:32])
    body = dci[32:]
    nblocks = struct.unpack_from('<H', entry, 0x18)[0]
    if entry[0] not in (0x33, 0xCC) or len(body) != nblocks * 512:
        raise SystemExit('%s: not a DCI save (type %02x, %d blocks, %d data bytes)' % (dci_path, entry[0], nblocks, len(body)))
    data = b''.join(body[i:i + 4][::-1] for i in range(0, len(body), 4))
    name = bytes(entry[4:16])

    fat = list(struct.unpack_from('<256H', card, FAT_BLOCK * 512))
    slots = [(b, e) for b in range(DIR_FIRST, DIR_LAST - 1, -1) for e in range(16)]

    def at(slot):
        return slot[0] * 512 + slot[1] * 32

    # remove a file of the same name
    for s in slots:
        o = at(s)
        if card[o] and card[o + 4:o + 16] == name:
            b = struct.unpack_from('<H', card, o + 2)[0]
            for _ in range(256):
                if b >= 256:
                    break
                nxt = fat[b]
                fat[b] = FREE
                if nxt == LAST:
                    break
                b = nxt
            card[o:o + 32] = bytes(32)
            print('removed the old %s' % name.decode('ascii', 'replace').strip())

    free = [b for b in range(USER_BLOCKS - 1, -1, -1) if fat[b] == FREE]
    if len(free) < nblocks:
        raise SystemExit('the card has %d free blocks, the save needs %d' % (len(free), nblocks))
    blocks = free[:nblocks]
    for i, b in enumerate(blocks):
        fat[b] = blocks[i + 1] if i + 1 < len(blocks) else LAST
        card[b * 512:(b + 1) * 512] = data[i * 512:(i + 1) * 512]
    struct.pack_into('<256H', card, FAT_BLOCK * 512, *fat)

    slot = next((s for s in slots if not card[at(s)]), None)
    if slot is None:
        raise SystemExit('the card directory is full')
    struct.pack_into('<H', entry, 2, blocks[0])
    card[at(slot):at(slot) + 32] = entry

    if os.path.exists(card_path):
        shutil.copyfile(card_path, card_path + '.bak')
    open(card_path, 'wb').write(card)
    print('copied %s (%d blocks, from block %d) into %s; the old card is %s.bak'
          % (name.decode('ascii', 'replace').strip(), nblocks, blocks[0], card_path, card_path))


if __name__ == '__main__':
    main()
