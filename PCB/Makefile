all:	png stl

stl:	Remote/Remote.stl Remote/RemoteCO2.stl Remote/RemoteBlind.stl $(patsubst %.scad,%.stl,$(wildcard *.scad))

PCBCase/clean: PCBCase/clean.c
	make -C PCBCase

PCBCase/case: PCBCase/case.c
	make -C PCBCase

%.png:	%.kicad_pcb PCBCase/clean PCBCase/render
	PCBCase/render $<

png:	$(patsubst %.kicad_pcb,%.png,$(wildcard */*.kicad_pcb))

%.stl: %.scad
	echo "Making $@"
	/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD $< -o $@
	echo "Made $@"

Remote/Remote.scad: Remote/Remote.kicad_pcb PCBCase/case Makefile
	PCBCase/case -o $@ $< --top=6 --bottom=2 --pcb=2 --case=3 --ignore=U2,J1.2

Remote/RemoteCO2.scad: Remote/Remote.kicad_pcb PCBCase/case Makefile
	PCBCase/case -o $@ $< --top=12 --bottom=2 --pcb=2 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7

Remote/RemoteBlind.scad: Remote/Remote.kicad_pcb PCBCase/case Makefile
	PCBCase/case -o $@ $< --top=12 --bottom=2 --pcb=2 --ignore=J1
