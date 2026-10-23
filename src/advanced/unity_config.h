#pragma once

// Send one character of Unity's output to the report, so it lands on the screen and in the log
void unity_output_char(int c);

// Route Unity's output through the report
#define UNITY_OUTPUT_CHAR(c) unity_output_char(c)

// Leave out the float assertions, since no request carries a float
#define UNITY_EXCLUDE_FLOAT

// Leave out the double assertions
#define UNITY_EXCLUDE_DOUBLE
