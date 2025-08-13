all:	png stl

stl:	Remote/RemoteCIBT.stl Remote/RemoteC--T.stl Remote/Remote-I-T.stl Remote/Remote--BT.stl Remote/RemoteCI-T.stl Remote/RemoteC-BT.stl Remote/Remote-IBT.stl Remote/Remote---T.stl Remote/RemoteBlindT.stl Remote/RemoteCIBB.stl Remote/RemoteC--B.stl Remote/Remote-I-B.stl Remote/Remote--BB.stl Remote/RemoteCI-B.stl Remote/RemoteC-BB.stl Remote/Remote-IBB.stl Remote/Remote---B.stl Remote/RemoteBlindB.stl $(patsubst %.scad,%.stl,$(wildcard *.scad))

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

Remote/RemoteBlindT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1 --case=3 --two-file

Remote/Remote---T.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 --case=3 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,U6,SW1 --two-file

Remote/RemoteC--T.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U6,SW1 --two-file

Remote/Remote-I-T.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,SW1 --two-file

Remote/Remote--BT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2,U6 --two-file

Remote/RemoteCI-T.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,SW1 --two-file

Remote/RemoteC-BT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U6 --two-file

Remote/Remote-IBT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7,U2 --two-file

Remote/RemoteCIBT.scad: Remote/Remote.kicad_pcb PCBCase/case
	PCBCase/case -o $@ $< --top=12 --bottom=2.5 --pcb=2 -y92.5 --ignore=J1.1,J1.3,J1.4,J1.5,J1.6,J1.7 --two-file


