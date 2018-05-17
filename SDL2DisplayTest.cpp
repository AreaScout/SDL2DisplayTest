// SDL2DisplayTest.cpp : Defines the entry point for the console application.
//

#include <algorithm>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <iterator>
#include <pthread.h>
#include <poll.h>
#include <sstream>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <omp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define WINDOW_WIDTH 320
#define WINDOW_HEIGHT 240

#define USERSWITCH "/sys/class/gpio/gpio24/value"
#define PIN 24

pthread_t tid_switch;
int fd_dt;
struct pollfd polls;
volatile bool running = false;
volatile bool draw_osd = false;
static char temp_buffer[256] = { 0 };
static char info_string[16][128] = {0};
static char ip_buffer[256] = {0};
int cpu[4], freq[8], gpu1, gpu_clock;
int usage[9] = {0};
int prev_total[9] = {0};
int prev_idle[9] = {0};
std::string gpu_string;
std::string connect_string;
std::string hdd_string;
std::string mem_string;
std::stringstream cpu_usage;
TTF_Font *font = NULL;

void *watchdog_thread(void *param)
{
	uint8_t c = 0xff;

	while(running) {
		int x =  poll (&polls, 1, -1);

		(void)read (fd_dt, &c, 1);
		lseek (fd_dt, 0, SEEK_SET);

		if(c == 0x30 || c == 0xff) {
			draw_osd = true;
		} else if(c == 0x31) {
			draw_osd = false;
		} else {
			c = 0xff;
		}
	}
}

void setup_gpio_pin()
{
	FILE *fd;

	if ((fd = fopen ("/sys/class/gpio/export", "w")) == NULL) {
		fprintf (stderr, "Unable to open GPIO export interface: %s\n", strerror (errno));
		exit(EXIT_FAILURE);
	}

	fprintf (fd, "%d\n", PIN);
	fclose (fd);

	if ((fd = fopen ("/sys/class/gpio/gpio24/direction", "w")) == NULL) {
		fprintf (stderr, "Unable to open GPIO direction interface for pin %d: %s\n", PIN, strerror (errno));
		exit(EXIT_FAILURE);
	}

	fprintf (fd, "in\n"); 
	fclose (fd);

	if ((fd = fopen ("/sys/class/gpio/gpio24/edge", "w")) == NULL) {
		fprintf (stderr, "Unable to open GPIO edge interface for pin %d: %s\n", PIN, strerror (errno));
		exit(EXIT_FAILURE);
	}

	// falling, rising, both
	fprintf (fd, "both\n");

	fclose (fd);
}

int get_ip (char *buf)
{
	struct  ifaddrs *ifa;
	int     str_cnt;

	getifaddrs(&ifa);

	while(ifa)  {
		if(ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)    {
			struct sockaddr_in *pAddr = (struct sockaddr_in *)ifa->ifa_addr;

			if(strncmp(ifa->ifa_name, "eth", 3) == 0) {
				str_cnt = sprintf(buf, "%s", inet_ntoa(pAddr->sin_addr));
			}
		}
		ifa = ifa->ifa_next;
	}
	freeifaddrs(ifa);

	return str_cnt;
}

void intHandler(int param) 
{
	running = false;
}

int calUsage(int cpu_idx, int user, int nice, int system, int idle, int wait, int irq, int srq)
{
	long total = 0;
	long usage = 0;
	int diff_idle, diff_total;

	total = user + nice + system + idle + wait + irq + srq;

	diff_idle  = idle - prev_idle[cpu_idx];
	diff_total = total - prev_total[cpu_idx];

	if (total != 0)
		// https://github.com/Leo-G/DevopsWiki/wiki/How-Linux-CPU-Usage-Time-and-Percentage-is-calculated
		usage = (1000 * (diff_total - diff_idle) / diff_total + 5) / 10;

	prev_total[cpu_idx] = total;
	prev_idle[cpu_idx] = idle;

	return usage;
}

SDL_Surface* CreateOSDandShadow(char *string)
{
	SDL_Color white = { 0xee, 0xfb, 0xff };
	SDL_Color black = { 0x00, 0x00, 0x00 };
	SDL_Surface *bg_surface = TTF_RenderText_Blended(font, string, black);
	SDL_Surface *fg_surface = TTF_RenderText_Blended(font, string, white);
	SDL_Rect rect = { 1, 1, fg_surface->w, fg_surface->h };

	// blit text on background surface 
	SDL_SetSurfaceBlendMode(fg_surface, SDL_BLENDMODE_BLEND);
	SDL_BlitSurface(fg_surface, NULL, bg_surface, &rect);
	SDL_FreeSurface(fg_surface);

	return bg_surface;
}

SDL_Surface* blitOSD(SDL_Surface *image)
{
	SDL_Rect dest = { 0 };
	dest.y = 0;
	
	for(int i = 0;i < 16; i++) {
		dest.w = image->clip_rect.w;
		dest.h = image->clip_rect.h;
		dest.x = 0;
		SDL_Surface *temp = CreateOSDandShadow(info_string[i]);
		SDL_BlitSurface(temp, NULL, image, &dest);
		SDL_FreeSurface(temp);
		dest.y += 15;
	}

	return image;
}

void fillInfoString()
{
	get_ip(ip_buffer);
	connect_string.erase(std::remove(connect_string.begin(), connect_string.end(), '\n'), connect_string.end());

	sprintf(info_string[0], "GPU : %s, %d\xBA""C, %dMHz", gpu_string.c_str(), gpu1, gpu_clock);
	sprintf(info_string[1], "%s", connect_string.c_str());
	sprintf(info_string[2], "Current IP : %s", ip_buffer);
	sprintf(info_string[3], "%s", hdd_string.c_str());
	sprintf(info_string[4], "%s", mem_string.c_str());
	sprintf(info_string[5], "Overall CPU usage : %s%s%2d%%", (usage[0] >= 100) ? "" : " ", (usage[0] >= 10) ? "" : " ", usage[0]);
	sprintf(info_string[6], "Cortex-A15:");
	sprintf(info_string[7], "CPU1 : %s%s%2d%%  %2d\xBA""C  %.2fGHz", (usage[5] >= 100) ? "" : " ", (usage[5] >= 10) ? "" : " ", usage[5], cpu[0], (float)freq[4] / 1000000);
	sprintf(info_string[8], "CPU2 : %s%s%2d%%  %2d\xBA""C  %.2fGHz", (usage[6] >= 100) ? "" : " ", (usage[6] >= 10) ? "" : " ", usage[6], cpu[1], (float)freq[5] / 1000000);
	sprintf(info_string[9], "CPU3 : %s%s%2d%%  %2d\xBA""C  %.2fGHz", (usage[7] >= 100) ? "" : " ", (usage[7] >= 10) ? "" : " ", usage[7], cpu[2], (float)freq[6] / 1000000);
	sprintf(info_string[10], "CPU4 : %s%s%2d%%  %2d\xBA""C  %.2fGHz", (usage[8] >= 100) ? "" : " ", (usage[8] >= 10) ? "" : " ", usage[8], cpu[3], (float)freq[7] / 1000000);
	sprintf(info_string[11], "Cortex-A7:");
	sprintf(info_string[12], "CPU1 : %s%s%2d%%  %.2fGHz", (usage[1] >= 100) ? "" : " ", (usage[1] >= 10) ? "" : " ", usage[1], (float)freq[0] / 1000000);
	sprintf(info_string[13], "CPU2 : %s%s%2d%%  %.2fGHz", (usage[2] >= 100) ? "" : " ", (usage[2] >= 10) ? "" : " ", usage[2], (float)freq[1] / 1000000);
	sprintf(info_string[14], "CPU3 : %s%s%2d%%  %.2fGHz", (usage[3] >= 100) ? "" : " ", (usage[3] >= 10) ? "" : " ", usage[3], (float)freq[2] / 1000000);
	sprintf(info_string[15], "CPU4 : %s%s%2d%%  %.2fGHz", (usage[4] >= 100) ? "" : " ", (usage[4] >= 10) ? "" : " ", usage[4], (float)freq[3] / 1000000);
}

void readSensors()
{
	FILE *fd;
	int lSize;
	char buf[80] = {0,};
	char cpuid[9] = "cpu";
	int user = 0, system = 0, nice = 0, idle = 0, wait = 0, irq = 0, srq = 0;
	int cpu_index = 0;
	char line[256] = {0};
	char hdd_info[5][15] = {0};
	char mem_info[9][30] = {0};
	char cmd[]="df /home -h";
	int count = 0;

	if ((fd = fopen ("/sys/devices/10060000.tmu/temp", "r")) == NULL) {
		fprintf (stderr, "Unable to open sysfs for temperature reading: %s\n", strerror (errno));
	}

	fseek (fd , 0 , SEEK_END);
	lSize = ftell (fd);
	rewind (fd);

	size_t result = fread(temp_buffer, 1, lSize, fd);

	std::string input(temp_buffer);

	std::stringstream stream(input.substr(10, 2));
	stream >> cpu[0];
	stream.str("");
	stream.clear();
	stream << input.substr(26, 2);
	stream >> cpu[1];
	stream.str("");
	stream.clear();
	stream << input.substr(42, 2);
	stream >> cpu[2];
	stream.str("");
	stream.clear();
	stream << input.substr(58, 2);
	stream >> cpu[3];
	stream.str("");
	stream.clear();
	stream << input.substr(74, 2);
	stream >> gpu1;
	stream.str("");
	stream.clear();

	fclose(fd);
	
	fd = fopen("/proc/stat", "r");
	if (fd == NULL)
		fprintf (stderr, "Unable to open proc file system: %s\n", strerror (errno));

	while (fgets(buf, 80, fd)) {
		if (!strncmp(buf, "cpu", 3)) {
			sscanf(buf, "%s %d %d %d %d", cpuid, &user, &nice, &system, &idle, &wait, &irq, &srq);
			usage[cpu_index] = calUsage(cpu_index, user, nice, system, idle, wait, irq, srq);
			// CPU Usage stringstream
			cpu_usage << ", " << cpuid << " " << usage[cpu_index] << "%";
			cpu_index++;
		}
		if (!strncmp(buf, "intr", 4))
			break;
	}
	fclose(fd);

	for (int i = 0; i < 8; i++) {
		char freq_path[60] = {0};
		sprintf(freq_path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
		fd = fopen(freq_path, "r");
		if (fd == NULL)
			fprintf (stderr, "Unable to open proc file system: %s\n", strerror (errno));

		fgets(line, 256 , fd);

		std::string _freq(line);
		std::stringstream ss_freq(_freq);
		// CPU Clock integer
		ss_freq >> freq[i];
		memset(line, 0, 256);
		fclose(fd);
	}

	fd = fopen("/proc/meminfo", "r");
	if (fd == NULL)
		fprintf (stderr, "Unable to open proc file system: %s\n", strerror (errno));

	std::stringstream mem_tmp;
	while (fgets(line, 256 , fd)) {
		mem_tmp << line;
		count++;
		if(count > 3)
			break;
	}
	memset(line, 0, 256);
	fclose(fd);

	mem_string = std::string(mem_tmp.str());

	char mem_str[256] = {0};
	sscanf(mem_string.c_str(), "%s %s %s %s %s %s %s %s %s", mem_info[0], mem_info[1], mem_info[2], mem_info[3], mem_info[4], mem_info[5], mem_info[6], mem_info[7], mem_info[8]);
	sprintf(mem_str, "Mem: %.1f/%.1f MiB (%.1f%)", (float)atoi(mem_info[4])/1000, (float)atoi(mem_info[1])/1000, ((float)atoi(mem_info[4])/1000) * 100.0f / ((float)atoi(mem_info[1])/1000));
	// Memory Info string
	mem_string = std::string(mem_str);

	fd = fopen("/sys/devices/11800000.mali/clock", "r");
	if (fd == NULL)
		fprintf (stderr, "Unable to open proc file system: %s\n", strerror (errno));

	fgets(line, 256 , fd);

	std::string gpuclock(line);
	std::stringstream gpustream(gpuclock);

	// GPU clock frequency integer
	gpustream >> gpu_clock;
	memset(line, 0, 256);
	fclose(fd);

	fd = fopen("/sys/devices/11800000.mali/gpuinfo", "r");
	if (fd == NULL)
        fprintf (stderr, "Unable to open sys file system: %s\n", strerror (errno));

	fgets(line, 256 , fd);

	// GPU Info string
	gpu_string = std::string(line).substr(0, 13);
	memset(line, 0, 256);
	fclose(fd);

	fd = fopen("/sys/devices/platform/exynos-drm/drm/card0/card0-HDMI-A-1/status", "r");
	if (fd == NULL)
		fprintf (stderr, "Unable to open sys file system: %s\n", strerror (errno));

	fgets(line, 256 , fd);

	// HDMI connect string 
	connect_string = "HDMI: " + std::string(line);
	memset(line, 0, 256);
	fclose(fd);

	count = 0;
	FILE* apipe = popen(cmd, "r");
	while (!feof(apipe)) {
		line[count] = fgetc(apipe);
		count++;
	}

	hdd_string = std::string(line).substr(48, std::string(line).size() - 48);
	sscanf(hdd_string.c_str(), "%s %s %s %s %s", hdd_info[0], hdd_info[1], hdd_info[2], hdd_info[3], hdd_info[4]); 
	char hdd_str[256] = {0};
	sprintf(hdd_str, "HDD: %siB (%s used) %s", hdd_info[1], hdd_info[4], hdd_info[0]);

	// HDD info string
	hdd_string = std::string(hdd_str);

	pclose(apipe);

	fillInfoString();
}

int main(int argc, char *argv[])
{
	int fd, i , it, count;
	uint8_t c;
	pid_t pid;
	SDL_Renderer *renderer = NULL;
	SDL_Window *window = NULL;
	SDL_Event event;
	SDL_Surface *surface = NULL;
	SDL_Surface *_surface = NULL;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;

	uint32_t rmask = 0x0000f800, gmask = 0x000007e0, bmask = 0x0000001f, amask = 0x00000000;
	uint32_t rmask32 = 0x0000ff00, gmask32 = 0x00ff0000, bmask32 = 0xff000000, amask32 = 0x000000ff;

	SDL_SetMainReady();

	if(argc < 3 && getuid() != 0) {
		printf("Usage: %s some.bmp some.ttf\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	// Run as deamon
	pid = fork();
	if (pid < 0)
		exit(EXIT_FAILURE);

	// Let the parent terminate
	if (pid > 0)
		exit(EXIT_SUCCESS);

	// The child process becomes session leader
	if (setsid() < 0)
		exit(EXIT_FAILURE);

	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	// Fork off a second time
	pid = fork();
	if (pid < 0)
		exit(EXIT_FAILURE);

	// Let the parent terminate
	if (pid > 0)
		exit(EXIT_SUCCESS);

	omp_set_dynamic(0);
	omp_set_num_threads(4); // Use 4 threads for all consecutive parallel regions

	signal(SIGINT, intHandler);

	// Configure the gpio pin device tree entries on the first run 
	if(access(USERSWITCH, F_OK) != -1) {
		if(getuid() == 0) {
			printf("Device tree entry is already set - exiting\n");
			exit(EXIT_SUCCESS);
		}
	} else {
		if(getuid() != 0) {
			printf("User has to be root to create gpio tree\n");
			exit(EXIT_FAILURE);
		}
		setup_gpio_pin();
		printf("All device tree entries created successfully - exiting\n");
		exit(EXIT_SUCCESS);
	}

	// Open device tree gpio entry
	if((fd_dt = open (USERSWITCH, O_RDONLY)) < 0) {
		printf("Unable to open gpio device tree entry\n");
		exit(EXIT_FAILURE);
	}

	// Clear any initial pending interrupt
	ioctl (fd_dt, FIONREAD, &count) ;
	for (i = 0 ; i < count ; ++i)
		read (fd_dt, &c, 1) ;

	// Setup poll structure
	polls.fd     = fd_dt;
	polls.events = POLLPRI;

	// Create the watchdog thread for the user switch
	running = true;
	int ret = pthread_create(&tid_switch, NULL, watchdog_thread, NULL);
	assert(!ret);

	// Initialize SDL2
	if (SDL_Init(SDL_INIT_EVENTS) < 0) {
		fprintf(stderr, "ERROR in SDL_Init(): %s\n", SDL_GetError());
		return 0;
	}

#if 0
	SDL_CreateWindowAndRenderer(WINDOW_WIDTH, WINDOW_WIDTH, 0, &window, &renderer);
#endif

	TTF_Init();
	font = TTF_OpenFont(argv[2], 15);
	if (font == NULL) {
		fprintf(stderr, "error: font not found\n");
		exit(EXIT_FAILURE);
	}

	SDL_Surface *image = SDL_LoadBMP(argv[1]);
	SDL_Rect imageRect = { 0, 0, 320, 240 };

	SDL_Surface *dark_surface = SDL_CreateRGBSurface(0, image->w, image->h, 32, rmask32, gmask32, bmask32, amask32);
	SDL_FillRect(dark_surface, NULL, SDL_MapRGBA(dark_surface->format, 0, 0, 0, 128));
	
	SDL_Surface *image_orig = SDL_CreateRGBSurface(image->flags, image->w, image->h, image->format->BytesPerPixel * 8, rmask, gmask, bmask, amask);
	
	// Copy the image
	SDL_BlitSurface(image, NULL, image_orig, NULL);	
	
	// Darken the BG Image
	SDL_BlitSurface(dark_surface, NULL, image, NULL);
	
	// Prepare sensor data for OSD
	readSensors();

	int pitch = image->pitch;
	int pxlength = pitch * image->h;
	unsigned char *pixels = NULL;
#if 0
	// Vertical flip the image data of the surface
	unsigned char *ppixels = (unsigned char*)surface->pixels;
	for (int line = 0; line < image->h; ++line) {
		memcpy(ppixels, pixels, pitch);
		pixels -= pitch;
		ppixels += pitch;
	}
#endif
#if 1
	if ((fd = open("/dev/fb1", O_RDWR)) < 0) {
		perror("can't open device");
		abort();
	}

	uint8_t *fbp = (uint8_t*)mmap(0, pxlength, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);

	unsigned char *pixels_orig = (unsigned char*)image_orig->pixels;

	while(running) {
		if(draw_osd) {
			// Blit/Update the OSD
			readSensors();
			SDL_FreeSurface(surface);
			SDL_FreeSurface(_surface);
			surface = SDL_CreateRGBSurface(image->flags, image->w, image->h, image->format->BytesPerPixel * 8, rmask, gmask, bmask, amask);
			_surface = SDL_CreateRGBSurface(image->flags, image->w, image->h, image->format->BytesPerPixel * 8, rmask, gmask, bmask, amask);
			SDL_BlitSurface(image, NULL, surface, NULL);
			SDL_BlitSurface(blitOSD(surface), NULL, _surface, NULL);
			pixels = (unsigned char*)_surface->pixels;
			#pragma omp for
			for (it = 0; it < pxlength; it++) {
				fbp[it] = pixels[it];
			}
			usleep(50000);
		} else if(!draw_osd) {
			#pragma omp for
			for (it = 0; it < pxlength; it++) {
				fbp[it] = pixels_orig[it];
			}
			usleep(1000000);
		}
	}
	close(fd);
	close(fd_dt);
#else
	// Create Texture from surface
	SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

	int quit = 0;
	while (!quit) {
		while (SDL_PollEvent(&event) == 1) {
			if (event.type == SDL_QUIT) {
				quit = 1;
			}
		}
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);

		/* Use TTF textures. */
		SDL_RenderCopy(renderer, texture, NULL, &imageRect);

		SDL_RenderPresent(renderer);
	}
#endif
    return 0;
}
