all:	png stl

stl:	Remote/Remote.stl $(patsubst %.scad,%.stl,$(wildcard *.scad))

PCBCase/clean: PCBCase/clean.c
	make -C PCBCase

PCBCase/case: PCBCase/case.c
	make -C PCBCase

%.png:	%.kicad_pcb PCBCase/clean PCBCase/render Makefile
	PCBCase/render $<

png:	$(patsubst %.kicad_pcb,%.png,$(wildcard */*.kicad_pcb))

%.stl: %.scad
	echo "Making $@"
	/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD $< -o $@
	echo "Made $@"

Remote/Remote.scad: Remote/Remote.kicad_pcb PCBCase/case Makefile
	PCBCase/case -o $@ $< --top=6 --bottom=2 --pcb=2 --case=3 --ignore=U2
