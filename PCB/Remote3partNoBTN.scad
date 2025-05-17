// Three part case
use <blankplate.scad>
use <LCD2.scad>
use <Remote/RemoteCO2.scad>
L=48;
W=58;
H=12;
wall=3;
CX=W/2;
CY=L/2;
pcb=1.2;
$fn=24;

difference()
{
    union()
    {
        translate([0,0,wall+H/2])
        minkowski()
        {
            cube([W,L,H],center=true);
            sphere(r=wall);
        }
    }
   translate([-CX-wall-1,-CY-wall-1,wall+pcb])
    cube([W+wall*2+2,L+wall*2+2,H+wall*2]);
   translate([0,0,wall+pcb-1+H/2])
   minkowski()
   {
    cube([W,L,H],center=true);
    cylinder(r=wall/2,h=2);
   }
   translate([-wall,wall,0])bottom_pos()
   {
    minkowski()
    {
        union()
        {
            pcb();
            parts_top(hole=true,part=true);
        }
        translate([-0.1,-0.1,0])cube([0.2,0.2,H]);
    }
   }
}

