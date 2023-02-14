#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

void grid2deg_( char *grid, double *dlong, double *dlat) {

    //! Converts Maidenhead grid locator to degrees of West longitude
    //! and North latitude.

    //char grid[7],g1,g2,g3,g4,g5,g6;
    char g1,g2,g3,g4,g5,g6;
    int nlong,n20d,nlat;
    double xminlong,xminlat;

    //strcpy(grid,grid0);

    g1=grid[0];
    g2=grid[1];
    g3=grid[2];
    g4=grid[3];
    g5=grid[4];
    g6=grid[5];

    // Regarding undeclared variables a Fortran tutorial says "all variables starting with the letters i-n are integers and all others are real".
    nlong = 180 - 20*((int)(g1-'A'));
    n20d = 2*((int)(g3-'0'));
    xminlong = 5*(double)(g5-'a')+0.5;
    *dlong = ((double)(nlong - n20d)) - xminlong/60.0;
    nlat = -90+10*(int)(g2-'A') + (int)(g4-'0');
    xminlat = 2.5*((double)(g6-'a')+0.5);
    *dlat = (double)nlat + xminlat/60.0;

}
