#!/usr/bin/env python3
"""Oracle hook: SC_WAV=<file> records the AICA's final mixed output (44,100 Hz stereo, 16-bit)
to a WAV file, for comparing the native port's sound with the emulator's.

Patches core/hw/aica/sgc_if.cpp (build_m0.sh restores it from git first)."""
import sys

src = sys.argv[1] + "/core/hw/aica/sgc_if.cpp"
s = open(src).read()
anchor = "\tWriteSample(mixr, mixl);"
assert s.count(anchor) == 1, "WriteSample call not found"
writer = r'''
// ---- SC oracle: SC_WAV records the mixed output ----
#include <cstdio>
#include <cstdlib>
namespace scwav
{
static FILE *f;
static unsigned bytes;
static void header()
{
	const unsigned rate = 44100, byteRate = rate * 4, riff = 36 + bytes, fmtLen = 16;
	const unsigned short pcm = 1, chans = 2, align = 4, bits = 16;
	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
	fwrite(&pcm, 2, 1, f); fwrite(&chans, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
	fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
	fseek(f, 0, SEEK_END);
}
static void put(int l, int r)
{
	static int init;
	if (!init)
	{
		init = 1;
		if (const char *n = getenv("SC_WAV"))
			if ((f = fopen(n, "wb")))
				header();
	}
	if (!f)
		return;
	short s[2] = { (short)l, (short)r };
	fwrite(s, 2, 2, f);
	bytes += 4;
	if ((bytes & 0xffff) == 0)
	{
		header();
		fflush(f);
	}
}
}
'''
s = s.replace(anchor, "\tscwav::put(mixl, mixr);\n" + anchor)
# the writer goes before the function that contains the anchor: after the last #include
last = s.rfind("#include")
eol = s.index("\n", last)
s = s[:eol + 1] + writer + s[eol + 1:]
open(src, "w").write(s)
print("SC wav hook applied to", src)
