#ifndef IO_H
#define IO_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned char insb(unsigned short port);
unsigned short insw(unsigned short port);


void outb(unsigned short port, unsigned char val);
void outw(unsigned short port, unsigned short val);

#ifdef __cplusplus
}
#endif

#endif // !IO_H