all:	png stl

stl:	Remote/Remote-PDCIB.stl $(patsubst %.scad,%.stl,$(wildcard */*.scad)) $(patsubst %.scad,%.stl,$(wildcard *.scad))

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

Remote/Remote-PDCIB.scad: Remote/Remote.kicad_pcb PCBCase/case Remote/makecase
	Remote/makecase Base
	Remote/makecase PDCIB
	Remote/makecase PDCI-
	Remote/makecase PDC-B
	Remote/makecase PDC--
	Remote/makecase PD-IB
	Remote/makecase PD-I-
	Remote/makecase PD--B
	Remote/makecase PD---
	Remote/makecase P-CIB
	Remote/makecase P-CI-
	Remote/makecase P-C-B
	Remote/makecase P-C--
	Remote/makecase P--IB
	Remote/makecase P--I-
	Remote/makecase P---B
	Remote/makecase P----
	Remote/makecase -DCIB
	Remote/makecase -DCI-
	Remote/makecase -DC-B
	Remote/makecase -DC--
	Remote/makecase -D-IB
	Remote/makecase -D-I-
	Remote/makecase -D--B
	Remote/makecase -D---
	Remote/makecase --CIB
	Remote/makecase --CI-
	Remote/makecase --C-B
	Remote/makecase --C--
	Remote/makecase ---IB
	Remote/makecase ---I-
	Remote/makecase ----B
	Remote/makecase -----
