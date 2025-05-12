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
     rotate([0,0,90])blankplate();
     translate([-2,0,8])cube([58+4,34.5+4,14],center=true);
   }
   translate([-2,0,15.2])cube([58+2,34.5+2,4],center=true);     
   top_pos()
   {
     part_U2(hole=true);
     part_SW1(hole=true);
     part_J1(hole=true);
     minkowski()
     {
        union()
        {
           part_J1();
           pcb();
        }
        translate([0,0,-10])cube([0.2,0.2,10]);
     }
   }  
}

translate([80,0,0])rotate([0,0,90])difference()
{
  translate([-2,6,1])cube([58+2,34.5+2,2],center=true);
  bottom_pos()
  {
    part_J1(hole=true);
  }
}