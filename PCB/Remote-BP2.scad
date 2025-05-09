use <blankplate.scad>
use <LCD2.scad>
use <Remote/RemoteCO2.scad>
W=34.5;
H=58;
M=4;
$fn=24;

difference()
{
    union()
    {
        blankplate();
        translate([-(W+M)/2,-(H+M)/2,0])cube([W+M,H+M,9]);
    }
    translate([0,0,10])rotate([180,0,90])lcd2(0.2);
    translate([-13-W/2,-H/2,11])rotate([0,180,-90])
    {
        minkowski()
        {
           pcb();
           translate([0,0,-10])cube([0.3,0.3,10]);
        }
        parts_top(part=true,hole=true);
    }
}
