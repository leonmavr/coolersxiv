/* Copyright 2011 Bert Muennich
 *
 * This file is part of sxiv.
 *
 * sxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * sxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with sxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sxiv.h"
#define _IMAGE_CONFIG
#include "config.h"
#include "version.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

opt_t _options;
const opt_t *options = (const opt_t*) &_options;

/*
 * True if s looks like "scheme://..." and is therefore a URL rather than a
 * local path. Only the scheme is required to be well-formed; everything after
 * "://" is left to the url handler.
 */
static bool is_url(const char *s)
{
	const char *p;

	if (s == NULL || !isalpha((unsigned char) *s))
		return false;
	for (p = s + 1; *p != '\0'; p++) {
		if (*p == ':')
			return p[1] == '/' && p[2] == '/';
		if (!(isalnum((unsigned char) *p) ||
		      *p == '+' || *p == '-' || *p == '.'))
			return false;
	}
	return false;
}

/* Remember a URL for later download by the external url handler. */
static void add_url(const char *url)
{
	_options.urls = erealloc(_options.urls, (_options.urlcnt + 1) * sizeof(char*));
	_options.urls[_options.urlcnt++] = estrdup(url);
}

void print_usage(void)
{
	printf("usage: sxiv [-bcfhiopqrRtvZ] [-A FRAMERATE] [-e WID] [-G GAMMA] "
	       "[-g GEOMETRY] [-N NAME] [-n NUM] [-S DELAY] [-s MODE] [-u URL]... "
	       "[-z ZOOM] FILES...\n");
}

void print_version(void)
{
	puts("sxiv " VERSION);
}

void parse_options(int argc, char **argv)
{
	int i, j, n, opt;
	char *end, *s;
	const char *scalemodes = "dfFwh";

	progname = strrchr(argv[0], '/');
	progname = progname ? progname + 1 : argv[0];

	_options.from_stdin = false;
	_options.to_stdout = false;
	_options.recursive = false;
	_options.reverse_sort = false;
	_options.startnum = 0;

	_options.urls = NULL;
	_options.urlcnt = 0;

	_options.scalemode = SCALE_DOWN;
	_options.zoom = 1.0;
	_options.animate = true;
	_options.gamma = 0;
	_options.slideshow = 0;
	_options.framerate = 0;

	_options.fullscreen = false;
	_options.embed = 0;
	_options.hide_bar = false;
	_options.geometry = NULL;
	_options.res_name = NULL;

	_options.quiet = false;
	_options.thumb_mode = false;
	_options.clean_cache = false;
	_options.private_mode = false;

	while ((opt = getopt(argc, argv, "A:bce:fG:g:hin:N:opqRrS:s:tu:vZz:")) != -1) {
		switch (opt) {
			case '?':
				print_usage();
				exit(EXIT_FAILURE);
			case 'A':
				n = strtol(optarg, &end, 0);
				if (*end != '\0' || n <= 0)
					error(EXIT_FAILURE, 0, "Invalid argument for option -A: %s", optarg);
				_options.framerate = n;
				/* fall through */
			case 'b':
				_options.hide_bar = true;
				break;
			case 'c':
				_options.clean_cache = true;
				break;
			case 'e':
				n = strtol(optarg, &end, 0);
				if (*end != '\0')
					error(EXIT_FAILURE, 0, "Invalid argument for option -e: %s", optarg);
				_options.embed = n;
				break;
			case 'f':
				_options.fullscreen = true;
				break;
			case 'G':
				n = strtol(optarg, &end, 0);
				if (*end != '\0')
					error(EXIT_FAILURE, 0, "Invalid argument for option -G: %s", optarg);
				_options.gamma = n;
				break;
			case 'g':
				_options.geometry = optarg;
				break;
			case 'h':
				print_usage();
				exit(EXIT_SUCCESS);
			case 'i':
				_options.from_stdin = true;
				break;
			case 'n':
				n = strtol(optarg, &end, 0);
				if (*end != '\0' || n <= 0)
					error(EXIT_FAILURE, 0, "Invalid argument for option -n: %s", optarg);
				_options.startnum = n - 1;
				break;
			case 'N':
				_options.res_name = optarg;
				break;
			case 'o':
				_options.to_stdout = true;
				break;
			case 'p':
				_options.private_mode = true;
				break;
			case 'q':
				_options.quiet = true;
				break;
			case 'r':
				_options.recursive = true;
				break;
			case 'R':
				_options.reverse_sort = true;
				break;
			case 'S':
				n = strtof(optarg, &end) * 10;
				if (*end != '\0' || n <= 0)
					error(EXIT_FAILURE, 0, "Invalid argument for option -S: %s", optarg);
				_options.slideshow = n;
				break;
			case 's':
				s = strchr(scalemodes, optarg[0]);
				if (s == NULL || *s == '\0' || strlen(optarg) != 1)
					error(EXIT_FAILURE, 0, "Invalid argument for option -s: %s", optarg);
				_options.scalemode = s - scalemodes;
				break;
			case 't':
				_options.thumb_mode = true;
				break;
			case 'u':
				/* Force the next argument to be treated as a URL. */
				add_url(optarg);
				break;
			case 'v':
				print_version();
				exit(EXIT_SUCCESS);
			case 'Z':
				_options.scalemode = SCALE_ZOOM;
				_options.zoom = 1.0;
				break;
			case 'z':
				n = strtol(optarg, &end, 0);
				if (*end != '\0' || n <= 0)
					error(EXIT_FAILURE, 0, "Invalid argument for option -z: %s", optarg);
				_options.scalemode = SCALE_ZOOM;
				_options.zoom = (float) n / 100.0;
				break;
		}
	}

	_options.filenames = argv + optind;
	_options.filecnt = argc - optind;

	if (_options.filecnt == 1 && STREQ(_options.filenames[0], "-")) {
		_options.filenames++;
		_options.filecnt--;
		_options.from_stdin = true;
	}

	/* Positional arguments that look like URLs are routed to the url handler,
	 * so "sxiv URL [URL...]" and mixed file/URL lists work without -u. */
	for (i = 0, j = 0; i < _options.filecnt; i++) {
		if (is_url(_options.filenames[i]))
			add_url(_options.filenames[i]);
		else
			_options.filenames[j++] = _options.filenames[i];
	}
	_options.filecnt = j;
}
