all:	png stl

stl:	Remote/RemoteCIBT.stl Remote/RemoteCIBB.stl Remote/RemoteCIB.stl Remote/RemoteC--.stl Remote/Remote-I-.stl Remote/Remote--B.stl Remote/RemoteCI-.stl Remote/RemoteC-B.stl Remote/Remote-IB.stl Remote/Remote---.stl Remote/RemoteBlind.stl $(patsubst %.scad,%.stl,$(wildcard *.scad))

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

Remote/RemoteBlind.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1 --case=3 --fit=0 --snap=0

Remote/Remote---.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 --case=3 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,U6,SW1 --fit=0 --snap=0

Remote/RemoteC--.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U6,SW1 --fit=0 --snap=0

Remote/Remote-I-.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,SW1 --fit=0 --snap=0

Remote/Remote--B.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,U6 --fit=0 --snap=0

Remote/RemoteCI-.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,SW1 --fit=0 --snap=0

Remote/RemoteC-B.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U6 --fit=0 --snap=0

Remote/Remote-IB.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2 --fit=0 --snap=0

Remote/RemoteCIB.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7 --fit=0 --snap=0

Remote/RemoteCIBT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7 -T

Remote/RemoteCIBB.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7 -B


