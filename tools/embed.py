#!/usr/bin/env python3
"""Embed binary file as C header array. Usage: embed.py <file> <name>"""
import sys
data = open(sys.argv[1], 'rb').read()
name = sys.argv[2]
print(f'/* {name} ({len(data)} bytes) */')
print(f'static const unsigned char {name}[] = {{')
for i in range(0, len(data), 16):
    chunk = ', '.join(f'0x{b:02x}' for b in data[i:i+16])
    print(f'    {chunk},')
print('};')
print(f'static const unsigned long {name}_size = {len(data)};')
