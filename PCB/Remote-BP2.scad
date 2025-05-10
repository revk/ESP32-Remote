use <blankplate.scad>
use <LCD2.scad>
use <Remote/RemoteNoCO2.scad>
W=34.5;
H=58;
M=4;
$fn=24;

difference()
{
   blankplate();
   cube([34.5,58,10],center=true);
   for(y=[0:5:20])translate([-30,y+10,-1])cylinder(d=3,h=10,$fn=24);
  # translate([-30,-25,-10])cylinder(d=8,h=20);
}
translate([20.5,-32,0])rotate([0,0,90])top();
translate([20.5+80,-32,0])rotate([0,0,90])bottom();