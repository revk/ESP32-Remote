use <blankplate.scad>
use <Remote/RemoteBlind.scad>
W=34.5;
H=58;
M=4;

difference()
{
    union()
    {
        blankplate();
        translate([-(W+M)/2,-(H+M)/2,0])cube([W+M,H+M,5]);
    }
    translate([-13-W/2,H/2,3])rotate([0,0,-90])minkowski()
    {
       pcb();
       cube([0.3,0.3,10]);
    }
}
