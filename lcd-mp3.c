// TODO
// - test out debouncing code

/*
 *	lcd-mp3
 *	mp3 player with output to a 16x2 lcd display for song information
 *
 *	Portions of this code were borrowed from MANY other projects including (but limited to)
 *	- wiringPi example code for lcd.c
 *	- http://hzqtc.github.io/2012/05/play-mp3-with-libmpg123-and-libao.html
 *	- http://www.arduino.cc/en/Tutorial/Debounce
 *	- many thanks to those who helped me out at StackExchange (http://raspberrypi.stackexchange.com/)
 *
 *      requires:
 *		ncurses
 *		pthread
 *		wiringPi
 *		ao
 *		mpg123
 *	John Wiggins (jcwiggi@gmail.com)
 *
 *
 *	29-11-2014	Tons of modifications:
 *			- added a mount/unmount USB (assuming /dev/sda1) option
 *			- added more argument options such as whole dir, and mount usb
 *			- worked with another way to debounce buttons
 *	28-11-2014	added some help
 *	25-11-2014	attempt to add button support
 *	22-11-2014	moved from an array of songs to a linked list (playlist) of songs.
 *	18-11-2014	lots of re-work, added more info to curses display and the ability to
 *			swap what is shown on the second row; album vs artist
 *	17-11-2014	attempt to add previous song
 *	17-11-2014	added quit
 *	16-11-2014	added skip song / next song
 *	14-11-2014	added ability to pause thread/song; using ncurses, got keboard commands
 *	12-11-2014	added playback of multiple songs
 *	10-11-2014	added ID3 parsing of MP3 file
 *	04-11-2014	made the song playing part a thread
 *	02-11-2014	able to play a mp3 file using mpg123 and ao libraries
 *	28-10-2014	worked on scrolling text on lcd
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <libgen.h>

// for mounting
#include <sys/mount.h>
#include <dirent.h> 

#include <wiringPi.h>
#include <lcd.h>

#include "lcd-mp3.h"

// Push button vars:
#define playButtonPin 0
#define prevButtonPin 1
#define nextButtonPin 2
#define infoButtonPin 5
#define quitButtonPin 7

const int buttonPins[] = {
	playButtonPin,
	prevButtonPin,
	nextButtonPin,
	infoButtonPin,
	quitButtonPin
	};

// Debouncer vars
struct button_info {
	int buttonState;	/* current reading from input pin */
	int lastButtonState;	/* previous reading from input */
	long lastDebounceTime;	/* last time button was toggled */
	int buttonType;		/* button is either play/prev/next/pause/info/quit */
	int pin;
}; typedef struct button_info tempButton;

// linked list / playlist functions

int playlist_init(playlist_t *playlistptr)
{
  *playlistptr = NULL;
  return 1;
}

int playlist_add_song(int index, void *songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev, *new;
  int found = FALSE;

  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      free(cur->songptr);
      cur->songptr = songptr;
      found = TRUE;
      break;
    }
    else if (cur->index > index)
      break;
  }
  if (!found)
  {
    new = (playlist_node_t *)malloc(sizeof(playlist_node_t));
    new->index = index;
    new->songptr = songptr;
    new->nextptr = cur;
    if (cur == *playlistptr)
      *playlistptr = new;
    else
      prev->nextptr = new;
  }
  return 1;
}

int playlist_get_song(int index, void **songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev;

  // Initialize to "not found"
  *songptr = NULL;
  // Look through index for our entry
  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      *songptr = cur->songptr;
      break;
    }
    else if (cur->index > index)
      break;
  }
  return 1;
}

/*
int playlist_song_count(playlist_t *playlistptr)
{
	playlist_node_t *cur;
	int cnt = 0;

	for (cur = *playlistptr; cur != NULL; cur = cur->nextptr)
		cnt++;
	cnt--; // get rid of the incrememnted value
	return cnt;
}
*/

// button debouncing stuff TODO
// NOTE:
//	The following was borrowed from http://www.arduino.cc/en/Tutorial/Debounce
//	which states the example is in the public domain; but I still want to give them props.
void deBouncer()
{
	struct button_info cur_button;
	int reading;
	
	cur_button = tempButton;
	reading = digitalRead(cur_button.pin);
	// check to see if you just pressed the button
	// (i.e. the input went from LOW to HIGH), and you've waited
	// long enough since the last press to ignore any noise:
	// if the swtich changed, due to noise or pressing:
	if (reading != cur_button.lastButtonState)
	{
		// reset the debouncing timer
		cur_button.lastDebounceTime = millis();
	}
	if ((millis() - cur_button.lastDebounceTime) > debounceDelay)
	{
		// whatever the reading is at, it's been there for longer than
		// the debounce delay, so take it as the actual current state.
		// if the button state has changed:
		if (reading != cur_button.buttonState)
			cur_button.buttonState = reading;
	}
	// save the reading.  Next time through the loop, it'll be the lastButtonState:
	cur_button.lastButtonState = reading;
	tempButton = cur_button;
}

// mount (if cmd == 1, do not attempt to unmount)
int mountToggle(int cmd, char *dir_name)
{
	if (mount("/dev/sda1", dir_name, "vfat", MS_RDONLY | MS_SILENT, "") == -1)
	{
		// if it is already mounted; then unmount it.
		if (errno == EBUSY && cmd == 2)
		{
			umount2("/MUSIC", MNT_FORCE);
			return UNMOUNTED;
		}
		// filesystem is already mounted so just return as mounted.
		else if (errno == EBUSY && cmd != 2)
			return MOUNTED;
		else
			return MOUNT_ERROR;
	}
	else
		return MOUNTED;
}

// if USB has been mounted, load in songs.
playlist_t reReadPlaylist(char *dir_name)
{
	int index;
	char *string;
	playlist_t new_playlist;
	DIR *d;
	struct dirent *dir;
	//char *dir_name = "/MUSIC";
	//int i=24;

	//printf("dir_name: %s\n", dir_name);
	index = 1;
	playlist_init(&new_playlist);
	d = opendir(dir_name);
	if (d)
	{
		while ((dir = readdir(d)) != NULL)
		{
			if (dir->d_type == 8)
			{
				string = malloc(MAXDATALEN);
				if (string == NULL)
					perror("malloc");
				strcpy(string, dir_name);
				strcat(string, "/");
				strcat(string, dir->d_name);
				//printf("song: %s\n", string);
				/*
				move(i, 0);
				printw("------------");
				move(i+1,0);
				printw("song: %s", string);
				i++;
				*/
				playlist_add_song(index++, string, &new_playlist);
			}
		}
	}
	closedir(d);
	pthread_mutex_lock(&cur_song.pauseMutex);
	num_songs = index;
	//printf("index: %d\n", index);
	pthread_mutex_unlock(&cur_song.pauseMutex);
	/*
	move(26, 0);
	if (index == 0)
	{
		printw("---- 0 --- songs: %d", index);
	}
	else
		printw("songs: %d", index);
	*/
	return new_playlist;
}

// pthread stuff
void nextSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = NEXT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void prevSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PREV;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void quitMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = QUIT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void pauseMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PAUSE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}


void playMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PLAY;
	pthread_cond_broadcast(&cur_song.m_resumeCond);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void checkPause()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	while (cur_song.play_status == PAUSE)
		pthread_cond_wait(&cur_song.m_resumeCond, &cur_song.pauseMutex);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

// ID3 stuff

// Split up a number of lines separated by \n, \r, both or just zero byte
//   and print out each line with specified prefix.
void make_id(mpg123_string *inlines, int type)
{
	size_t i;
	int hadcr = 0, hadlf = 0;
	char *lines = NULL;
	char *line  = NULL;
	size_t len = 0;
	char tmp_name[100];

	if (inlines != NULL && inlines->fill)
	{
		lines = inlines->p;
		len = inlines->fill;
	}
	else
		return;
	line = lines;
	for (i = 0; i < len; ++i)
	{
		if (lines[i] == '\n' || lines[i] == '\r' || lines[i] == 0)
		{
			// saving, changing, restoring a byte in the data
			char save = lines[i];
			if (save == '\n')
				++hadlf;
			if (save == '\r')
				++hadcr;
			if ((hadcr || hadlf) && hadlf % 2 == 0 && hadcr % 2 == 0)
				line = "";
			if (line)
			{
				lines[i] = 0;
				strncpy(tmp_name, line, 100);
				line = NULL;
				lines[i] = save;
			}
		}
		else
		{
			hadlf = hadcr = 0;
			if (line == NULL)
				line = lines + i;
		}
	}
	switch (type)
	{
		case  TITLE: strcpy(cur_song.title,  tmp_name); break;
		case ARTIST: strcpy(cur_song.artist, tmp_name); break;
		case  GENRE: strcpy(cur_song.genre,  tmp_name); break;
		case  ALBUM: strcpy(cur_song.album,  tmp_name); break;
	}
}

int id3_tagger()
{
	int meta;
	mpg123_handle* m;
	mpg123_id3v1 *v1;
	mpg123_id3v2 *v2;

	// ID3 tag info for the song
	mpg123_init();
	m = mpg123_new(NULL, NULL);
	if (mpg123_open(m, cur_song.filename) != MPG123_OK)
	{
		fprintf(stderr, "Cannot open %s: %s\n", cur_song.filename, mpg123_strerror(m));
		return 1;
	}
	mpg123_scan(m);
	meta = mpg123_meta_check(m);
	if (meta & MPG123_ID3 && mpg123_id3(m, &v1, &v2) == MPG123_OK)
	{
		make_id(v2->title, TITLE);
		make_id(v2->artist, ARTIST);
		make_id(v2->album, ALBUM);
		make_id(v2->genre, GENRE);
	}
	else
	{
		sprintf(cur_song.title, "UNKNOWN");
		sprintf(cur_song.artist, "UNKNOWN");
		sprintf(cur_song.album, "UNKNOWN");
		sprintf(cur_song.genre, "UNKNOWN");
	}
	// if there is no title to be found, set title to the song file name.
	if (strlen(cur_song.title) == 0)
		strcpy(cur_song.title, cur_song.base_filename);
	if (strlen(cur_song.artist) == 0)
		sprintf(cur_song.artist, "UNKNOWN");
	if (strlen(cur_song.album) == 0)
		sprintf(cur_song.album, "UNKNOWN");
	// set the second row to be the artist by default.
	strcpy(cur_song.second_row_text, cur_song.artist);
	mpg123_close(m);
	mpg123_delete(m);
	mpg123_exit();
	// the following two lines are just to see when the scrolling should pause
	strncpy(cur_song.scroll_firstRow, cur_song.title, 15);
	strncpy(cur_song.scroll_secondRow, cur_song.second_row_text, 16);
	return 0;
}

int printLcdFirstRow()
{
	int flag = TRUE;
	// have to set to 15 because of music note
	if (strlen(cur_song.title) < 15)
	{
		lcdCharDef(lcdHandle, 2, musicNote);
		lcdPosition(lcdHandle, 0, 0);
		lcdPutchar(lcdHandle, 2);
		lcdPosition(lcdHandle, 1, 0);
		lcdPuts(lcdHandle, cur_song.title);
		flag = FALSE;
	}
	return flag;
}

int printLcdSecondRow()
{
	int flag = TRUE;
	if (strlen(cur_song.second_row_text) < 16)
	{
		lcdPosition(lcdHandle, 0, 1);
		lcdPuts(lcdHandle, cur_song.second_row_text);
		flag = FALSE;
	}
	return flag;
}

int usage(const char *progName)
{
	fprintf(stderr, "Usage: %s [OPTION] "
		"--help "
		"-usb [mount] "
		"-dir [dir] "
		"-songs [MP3 files]\n", progName);
	return EXIT_FAILURE;
}

void scrollMessage_firstRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 15;
	char my_songname[MAXDATALEN];

	strcpy(my_songname, spaces);
	strncat(my_songname, cur_song.title, strlen(cur_song.title));
	strcat(my_songname, spaces);
	my_songname[strlen(my_songname) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_songname[position], width);
	buf[width] = 0;
	lcdCharDef(lcdHandle, 2, musicNote);
	lcdPosition(lcdHandle, 0, 0);
	lcdPutchar(lcdHandle, 2);
	lcdPosition(lcdHandle, 1, 0);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_firstRow) == 0)
		delay(1500);
	if (position == (strlen(my_songname) - width))
		position = 0;
}

void scrollMessage_secondRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 16;
	char my_string[MAXDATALEN];

	strcpy(my_string, spaces);
	strncat(my_string, cur_song.second_row_text, strlen(cur_song.second_row_text));
	strcat(my_string, spaces);
	my_string[strlen(my_string) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_string[position], width);
	buf[width] = 0;
	lcdPosition(lcdHandle, 0, 1);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_secondRow) == 0)
		delay(1500);
	if (position == (strlen(my_string) - width))
		position = 0;
}

// The actual thing that plays the song
void *play_song(void *arguments)
{
	struct song_info *args = (struct song_info *)arguments;
	mpg123_handle *mh;
	mpg123_pars *mpar;
	unsigned char *buffer;
	size_t buffer_size;
	size_t done;
	int err;

	int driver;
	ao_device *dev;
	ao_sample_format format;
	int channels, encoding;
	long rate;

	ao_initialize();
	driver = ao_default_driver_id();
	mpg123_init();
	// try to not show error messages
	mh = mpg123_new(NULL, &err);
	mpar = mpg123_new_pars(&err);
	mpg123_par(mpar, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	mh = mpg123_parnew(mpar, NULL, &err);
	buffer_size = mpg123_outblock(mh);
	buffer = (unsigned char*) malloc(buffer_size * sizeof(unsigned char));
	// open the file and get the decoding format
	mpg123_open(mh, args->filename);
	mpg123_getformat(mh, &rate, &channels, &encoding);
	// set the output format and open the output device
	format.bits = mpg123_encsize(encoding) * 8;
	format.rate = rate;
	format.channels = channels;
	format.byte_format = AO_FMT_NATIVE;
	format.matrix = 0;
	dev = ao_open_live(driver, &format, NULL);
	// decode and play
	while (mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK)
	{
		checkPause();
		ao_play(dev, buffer, done);
		// stop playing if the user pressed quit, next, or prev buttons
		if (cur_song.play_status == QUIT || cur_song.play_status == NEXT || cur_song.play_status == PREV)
			break;
	}
	// clean up
	free(buffer);
	ao_close(dev);
	mpg123_close(mh);
	mpg123_delete(mh);
	mpg123_exit();
	ao_shutdown();
	pthread_mutex_lock(&(cur_song.writeMutex));
	args->song_over = TRUE;
	// only set the status to play if the song finished normally
	if (cur_song.play_status != QUIT && cur_song.play_status != NEXT && cur_song.play_status != PREV)
		args->play_status = PLAY;
	cur_status.song_over = TRUE;
	pthread_mutex_unlock(&(cur_song.writeMutex));
}

int main(int argc, char **argv)
{
	pthread_t song_thread;
	playlist_t cur_playlist;
	struct button_info play_b;
	struct button_info prev_b;
	struct button_info next_b;
	struct button_info info_b;
	struct button_info quit_b;
	char *basec, *bname;
	char *string;
	char lcd_clear[] = "                ";
	int index;
	int song_index;
	int key;
	int i;
	int LCD_ONLY = FALSE;
	int mountFlag = UNMOUNTED;
	int useButtonFlag = FALSE;
	int scroll_firstRow_flag, scroll_secondRow_flag;

	// Initializations
	play_b.buttonType = PLAY;
	prev_b.buttonType = PREV;
	next_b.buttonType = NEXT;
	info_b.buttonType = INFO;
	quit_b.buttonType = QUIT;
	play_b.pin = playButtonPin;
	prev_b.pin = prevButtonPin;
	next_b.pin = nextButtonPin;
	info_b.pin = infoButtonPin;
	quit_b.pin = quitButtonPin;
	play_b.lastButtonState = LOW;
	prev_b.lastButtonState = LOW;
	next_b.lastButtonState = LOW;
	info_b.lastButtonState = LOW;
	quit_b.lastButtonState = LOW;
	play_b.lastDebounceTime = 0;
	prev_b.lastDebounceTime = 0;
	next_b.lastDebounceTime = 0;
	info_b.lastDebounceTime = 0;
	quit_b.lastDebounceTime = 0;
	playlist_init(&cur_playlist);
	//init_song(cur_song);
	cur_song.song_over = FALSE;
	scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0)
		{
			printf("Pins for buttons:\nButton Function\twiringPi\tBCM\n"
			       "Play\t0\t17\n"
			       "Prev\t1\t18\n"
			       "Next\t2\t27\n"
			       "Info\t5\t25\n"
			       "Quit\t7\t4\n");
			       return 1;
		}
		else if (strcmp(argv[1], "-songs") == 0)
		{
			for (index = 2; index < argc; index++)
			{
				string = malloc(MAXDATALEN);
				if (string == NULL)
					perror("malloc");
				strcpy(string, argv[index]);
				//printf("%d %d %s\n", argc, index, argv[index]);
				playlist_add_song(index - 1, string, &cur_playlist);
				num_songs = argc - 2;
				//printf("%d\n", zzz++);
			}
		}
		else if (strcmp(argv[1], "-usb") == 0)
		{
			mountFlag = mountToggle(1, argv[2]);
			if (mountFlag == MOUNTED)
			{
				//printf("arg2: %s\n", argv[2]);
				cur_playlist = reReadPlaylist(argv[2]);
				if (num_songs == 0)
				{
					printf("No songs found in mount %s\n", argv[2]);
					return -1;
				}
			}
			else if (mountFlag == MOUNT_ERROR)
			{
				printf("Mount Error - Can not mount %s\n", argv[2]);
				return -2;
			}
		}
		else if (strcmp(argv[1], "-dir") == 0)
		{
			cur_playlist = reReadPlaylist(argv[2]);
			if (num_songs == 0)
			{
				printf("No songs found in directory %s\n", argv[2]);
				return -1;
			}
		}
	}
	else
		return usage(argv[0]);
#ifdef fubarrrr
	for (index = 1; index < num_songs; index++)
	{
		string = malloc(MAXDATALEN);
		if (string == NULL)
			perror("malloc");
		playlist_get_song(index, &string, &cur_playlist);
		if (string != NULL)
		{
			printf("%d: %s\n", index, string);
		}
	}
#endif
//#else
//#ifdef RUNME
  	if (wiringPiSetup () == -1)
  	{
	    fprintf(stdout, "oops: %s\n", strerror(errno));
	    return 1;
	}
	for (i = 0; i < 5; i++)
	{
		pinMode(buttonPins[i], INPUT);
		pullUpDnControl(buttonPins[i], PUD_UP);
	}
	/*
	int t_ret;
	t_ret = piThreadCreate(songThread);
	*/
	// First test at button pausing/playing
	//wiringPiISR(playButtonPin, INT_EDGE_FALLING, &playSong);
	//wiringPiISR(nextButtonPin, INT_EDGE_RISING, &nextSong);
	//wiringPiISR(prevButtonPin, INT_EDGE_RISING, &prevSong);
	//wiringPiISR(stopButtonPin, INT_EDGE_RISING, &stopSong);
	/* =================================================END TODO=================================== */
	lcdHandle = lcdInit(RO, CO, BS, RS, EN, D0, D1, D2, D3, D0, D1, D2, D3);
	if (lcdHandle < 0)
	{
		fprintf(stderr, "%s: lcdInit failed\n", argv[0]);
		return -1;
	}
	// ncurses stuff (to be removed in final product)
	if (LCD_ONLY == FALSE)
	{
		initscr();
		noecho();
		cbreak();
		nodelay(stdscr, true);
		curs_set(0);
		printw("--= Pi LCD-MP3 Player =--");
		mvaddstr(2, 0, "by John Wiggins");
		mvaddstr(3, 0, "Controls:");
		//mvaddstr(4, 0, "           m - (un)mount USB toggle");
		mvaddstr(5, 0, "           n - next song");
		mvaddstr(6, 0, "           p - previous song");
		mvaddstr(7, 0, "       space - pause/play song");
		mvaddstr(8, 0, "           i - toggle info");
		mvaddstr(9, 0, "           q - quit program");
		refresh();
	}
	song_index = 1;
	cur_song.play_status = PLAY;
	//cur_song.num_songs = argc;
	while (cur_song.play_status != QUIT && song_index < num_songs)
	{
		if (LCD_ONLY == FALSE)
		{
			// Clear the lines for song title, artist, album - ncurses
			int clear_i;
			for (clear_i = 10; clear_i < 16; clear_i++)
				mvaddstr(clear_i, 0,   "                                                                     ");
		}
		string = malloc(MAXDATALEN);
		if (string == NULL)
			perror("malloc");
		playlist_get_song(song_index, &string, &cur_playlist);
		if (string != NULL)
		{
			basec = strdup(string);
			bname = basename(basec);
			strcpy(cur_song.filename, string);
			strcpy(cur_song.base_filename, bname);
			// See if we can get the song info from the file.
			id3_tagger();
			if (LCD_ONLY == FALSE)
			{
				//move(1, 0);
				//printw((mountFlag == MOUNTED ? "  Mounted" : "Unmounted"));
				move(10, 0);
				printw("------------------");
				move(11, 0);
				printw("Song Number: %d / %d", song_index, num_songs);
				move(12, 0);
				printw(" Song Title: %s", cur_song.title);
				move(13, 0);
				printw("     Artist: %s", cur_song.artist);
				move(14, 0);
				printw("      Album: %s", cur_song.album);
				move(15, 0);
				printw("      Genre: %s", cur_song.genre);
				refresh();
			}
			// play the song as a thread
			pthread_create(&song_thread, NULL, (void *) play_song, (void *) &cur_song);
			// Should we detach the thread? ... TODO
			//pthread_detach(song_thread);
			// The following displays stuff to the LCD without scrolling
			scroll_firstRow_flag = printLcdFirstRow();
			scroll_secondRow_flag = printLcdSecondRow();
			// loop to play the song
			while (cur_song.song_over == FALSE)
			{
				// Following code is to scroll the song info
				if (scroll_firstRow_flag == TRUE)
					scrollMessage_firstRow();
				if (scroll_secondRow_flag == TRUE)
					scrollMessage_secondRow();
				if (useButtonFlag == TRUE)
				{
					tempButton = play_b;
					deBouncer();
					play_b = tempButton;
					if (play_b.buttonState == LOW)
					{
						if (cur_song.play_status == PAUSE)
							playMe();
						else
							pauseMe();
					}
					tempButton = prev_b;
					deBouncer();
					prev_b = tempButton;
					if (prev_b.buttonState == LOW)
					{
						if (song_index - 1 != 0)
						{
							prevSong();
							song_index--;
						}
					}
					tempButton = next_b;
					deBouncer();
					next_b = tempButton;
					if (next_b.buttonState == LOW)
					{
						if (song_index + 1 < num_songs)
						{
							nextSong();
							song_index++;
						}
					}
					tempButton = info_b;
					deBouncer();
					info_b = tempButton;
					if (info_b.buttonState == LOW)
					{
						// toggle what to display
						pthread_mutex_lock(&cur_song.pauseMutex);
						strcpy(cur_song.second_row_text, (strcmp(cur_song.second_row_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
						// first clear just the second row, then re-display the second row
						lcdPosition(lcdHandle, 0, 1);
						lcdPuts(lcdHandle, lcd_clear);
						scroll_secondRow_flag = printLcdSecondRow();
						pthread_mutex_unlock(&cur_song.pauseMutex);
					}
					tempButton = quit_b;
					deBouncer();
					quit_b = tempButton;
					if (quit_b.buttonState == LOW)
					{
						quitMe();
					}
				}
				if (LCD_ONLY == FALSE)
				{
					key = getch();
					if (key > -1)
					{
						if (key == 'n')
						{
							// don't go to next song if last song
							// TODO maybe just quit the program if it is the last song...
							if (song_index + 1 < num_songs)
							{
								nextSong();
								song_index++;
							}
						}
						else if (key == 'p')
						{
							// don't go back if at first song
							if (song_index - 1 != 0)
							{
								prevSong();
								song_index--;
							}
						}
						else if (key == 'q')
						{
							quitMe();
						}
						else if (key == 'i')
						{
							// toggle what to display
							pthread_mutex_lock(&cur_song.pauseMutex);
							strcpy(cur_song.second_row_text, (strcmp(cur_song.second_row_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
							// first clear just the second row, then re-display the second row
							lcdPosition(lcdHandle, 0, 1);
							lcdPuts(lcdHandle, lcd_clear);
							scroll_secondRow_flag = printLcdSecondRow();
							pthread_mutex_unlock(&cur_song.pauseMutex);
						}
						/*
						else if (key == 'm')
						{
							mountFlag = mountToggle(2, "/MUSIC");
							//move(20, 0); printw("mountflag: %d", mountFlag);
							if (mountFlag == MOUNTED)
							{
								//pthread_mutex_lock(&cur_song.pauseMutex);
								tmp_playlist = reReadPlaylist("/MUSIC");
								if (num_songs != 0)
								{
									cur_playlist = tmp_playlist;
									cur_song.song_over = TRUE;
									song_index = 1;
								}
								//move(21, 0); printw("after readplaylist");
								//pthread_mutex_unlock(&cur_song.pauseMutex);
							}
							// FIXME maybe pause everything until it is mounted?
							else if (mountFlag == UNMOUNTED)
							{
								//pthread_mutex_lock(&cur_song.pauseMutex);
								cur_playlist = reReadPlaylist("/root/Music");
								cur_song.song_over = TRUE;
								song_index = 1;
								//pthread_mutex_unlock(&cur_song.pauseMutex);
							}
							else // FIXME
							{
								//pthread_mutex_lock(&cur_song.pauseMutex);
								cur_playlist = reReadPlaylist("/root/Music");
								cur_song.song_over = TRUE;
								song_index = 1;
								//pthread_mutex_unlock(&cur_song.pauseMutex);
							}
						}
						*/
						else if (key == ' ')
						{
							if (cur_song.play_status == PAUSE)
								playMe();
							else
								pauseMe();
						}
					}
				}
			}
			if (pthread_join(song_thread, NULL) != 0)
				perror("join error\n");
			// clear the lcd for next song.
			lcdClear(lcdHandle);
		}
		lcdClear(lcdHandle);
		// reset all the flags.
		scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
		// increment the song_index if the song is over but the next/prev wasn't hit
		if (cur_song.song_over == TRUE && cur_song.play_status == PLAY)
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
			song_index++;
		} // FIXME change mountFlag to cur_song.play_status == NEW
		//else if (cur_song.song_over == TRUE && (mountFlag == MOUNTED || (cur_song.play_status == NEXT || cur_song.play_status == PREV)))
		else if (cur_song.song_over == TRUE && (cur_song.play_status == NEXT || cur_song.play_status == PREV))
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			// empty out song/artist data
			strcpy(cur_song.title, "");
			strcpy(cur_song.artist, "");
			strcpy(cur_song.album, "");
			cur_song.play_status = PLAY;
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
		}
	}
	lcdClear(lcdHandle);
	lcdPosition(lcdHandle, 0, 0);
	lcdPuts(lcdHandle, "Good Bye!");
	delay(1000);
	lcdClear(lcdHandle);
	if (LCD_ONLY == FALSE)
		endwin();
//#endif
	return 0;
}
