// #include <stdio.h>
#include "./stdio.h"

/// @brief Write a line or string to the stdout
/// @param string 
/// @return the string
int puts(const char* string) {
	return printf("%s\n", string);
}
