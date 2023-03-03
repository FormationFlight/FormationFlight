#ifndef HELPERS_H
#define HELPERS_H

#define M_PI 3.14159265358979323846
double deg2rad(double deg);
double rad2deg(double rad);
double gpsDistanceBetween(double lat1d, double lon1d, double lat2d, double lon2d);
double gpsCourseTo(double lat1, double long1, double lat2, double long2);

#endif