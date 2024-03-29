/*
    libparted - a library for manipulating disk partitions
    Copyright (C) 1999-2000, 2002, 2007-2010 Free Software Foundation,
    Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* It's a bit silly calling a swap partition a file system.  Oh well...  */

#include <config.h>

#include <parted/parted.h>
#include <parted/endian.h>

#if ENABLE_NLS
#  include <libintl.h>
#  define _(String) dgettext (PACKAGE, String)
#else
#  define _(String) (String)
#endif /* ENABLE_NLS */

#include <unistd.h>
#include <uuid/uuid.h>

#define SWAP_SPECIFIC(fs) ((SwapSpecific*) (fs->type_specific))
#define BUFFER_SIZE 128

#define LINUXSWAP_BLOCK_SIZES       ((int[2]){512, 0})

typedef struct {
	char		page_map[1];
} SwapOldHeader;

/* ripped from mkswap */
typedef struct {
        char            bootbits[1024];    /* Space for disklabel etc. */
        uint32_t        version;
        uint32_t        last_page;
        uint32_t        nr_badpages;
        unsigned char   sws_uuid[16];
        unsigned char   sws_volume[16];
        uint32_t        padding[117];
        uint32_t        badpages[1];
} SwapNewHeader;

typedef struct {
	union {
		SwapNewHeader	new;
		SwapOldHeader	old;
	}* header;

	void*		buffer;
	int		buffer_size;

	PedSector	page_sectors;
	unsigned int	page_count;
	unsigned int	version;
	unsigned int	max_bad_pages;
} SwapSpecific;

static PedFileSystemType _swap_v0_type;
static PedFileSystemType _swap_v1_type;
static PedFileSystemType _swap_swsusp_type;

static PedFileSystem* _swap_v0_open (PedGeometry* geom);
static PedFileSystem* _swap_v1_open (PedGeometry* geom);
static PedFileSystem* _swap_swsusp_open (PedGeometry* geom);
static int swap_close (PedFileSystem* fs);

static PedGeometry*
_generic_swap_probe (PedGeometry* geom, int kind)
{
	PedFileSystem*	fs;
	SwapSpecific*	fs_info;
	PedGeometry*	probed_geom;
	PedSector	length;

        /* Fail the swap-file-system-recognizing test when sector size
           is not the default.  */
	if (geom->dev->sector_size != PED_SECTOR_SIZE_DEFAULT)
		return NULL;

        switch (kind) {
        /* Check for old style swap partitions. */
                case 0:
                        fs = _swap_v0_open(geom);
                        break;
        /* Check for new style swap partitions. */
                case 1:
                        fs = _swap_v1_open(geom);
                        break;
        /* Check for swap partitions containing swsusp data. */
                case -1:
                        fs = _swap_swsusp_open(geom);
                        break;
        /* Not reached. */
                default:
                        goto error;
        }

	if (!fs)
		goto error;
	fs_info = SWAP_SPECIFIC (fs);

	if (fs_info->version)
		length = fs_info->page_sectors * fs_info->page_count;
	else
	        length = geom->length;

	probed_geom = ped_geometry_new (geom->dev, geom->start, length);
	if (!probed_geom)
		goto error_close_fs;
	swap_close (fs);
	return probed_geom;

error_close_fs:
	swap_close (fs);
error:
	return NULL;
}

#ifndef DISCOVER_ONLY
static int
_generic_swap_clobber (PedGeometry* geom, int kind)
{
	PedFileSystem*		fs;
	char			buf[512];

        switch (kind) {
        /* Check for old style swap partitions. */
                case 0:
                        fs = _swap_v0_open(geom);
                        break;
        /* Check for new style swap partitions. */
                case 1:
                        fs = _swap_v1_open(geom);
                        break;
        /* Check for swap partitions containing swsusp data. */
                case -1:
                        fs = _swap_swsusp_open(geom);
                        break;
        /* Not reached */
                default:
                        goto error;
        }

	if (!fs)
		return 1;

	memset (buf, 0, 512);
	if (!ped_geometry_write (geom, buf, getpagesize() / 512 - 1, 1))
		goto error_close_fs;

	swap_close (fs);
	return 1;

error_close_fs:
	swap_close (fs);
error:
	return 0;
}
#endif /* !DISCOVER_ONLY */

static int
swap_init (PedFileSystem* fs, int fresh)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);

	fs_info->page_sectors = getpagesize () / 512;
	fs_info->page_count = fs->geom->length / fs_info->page_sectors;
	fs_info->version = 1;
	fs_info->max_bad_pages = (getpagesize()
					- sizeof (SwapNewHeader)) / 4;

	if (fresh) {
		uuid_t uuid_dat;

		memset (fs_info->header, 0, getpagesize());

		/* version is always 1 here */
		uuid_generate (uuid_dat);
		memcpy (fs_info->header->new.sws_uuid, uuid_dat,
			sizeof (fs_info->header->new.sws_uuid));
                return 1;
        }
	else
                return ped_geometry_read (fs->geom, fs_info->header,
                                          0, fs_info->page_sectors);
}

static PedFileSystem*
swap_alloc (PedGeometry* geom)
{
	PedFileSystem*	fs;
	SwapSpecific*	fs_info;

	fs = (PedFileSystem*) ped_malloc (sizeof (PedFileSystem));
	if (!fs)
		goto error;

	fs->type_specific = (SwapSpecific*) ped_malloc (sizeof (SwapSpecific));
	if (!fs->type_specific)
		goto error_free_fs;

	fs_info = SWAP_SPECIFIC (fs);
	fs_info->header = ped_malloc (getpagesize());
	if (!fs_info->header)
		goto error_free_type_specific;

	fs_info = SWAP_SPECIFIC (fs);
	fs_info->buffer_size = getpagesize() * BUFFER_SIZE;
	fs_info->buffer = ped_malloc (fs_info->buffer_size);
	if (!fs_info->buffer)
		goto error_free_header;

	fs->geom = ped_geometry_duplicate (geom);
	if (!fs->geom)
		goto error_free_buffer;
	fs->type = &_swap_v1_type;
	return fs;

error_free_buffer:
	free (fs_info->buffer);
error_free_header:
	free (fs_info->header);
error_free_type_specific:
	free (fs->type_specific);
error_free_fs:
	free (fs);
error:
	return NULL;
}

static void
swap_free (PedFileSystem* fs)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);

	free (fs_info->buffer);
	free (fs_info->header);
	free (fs->type_specific);

	ped_geometry_destroy (fs->geom);
	free (fs);
}

static PedFileSystem*
_swap_v0_open (PedGeometry* geom)
{
	PedFileSystem*		fs;
	SwapSpecific*		fs_info;
	const char*		sig;

	fs = swap_alloc (geom);
	if (!fs)
		goto error;
	swap_init (fs, 0);

	fs_info = SWAP_SPECIFIC (fs);
	if (!ped_geometry_read (fs->geom, fs_info->header, 0,
				fs_info->page_sectors))
		goto error_free_fs;

	sig = ((char*) fs_info->header) + getpagesize() - 10;
	if (strncmp (sig, "SWAP-SPACE", 10) == 0) {
		fs_info->version = 0;
		fs_info->page_count
			= PED_MIN (fs->geom->length / fs_info->page_sectors,
				   8 * (getpagesize() - 10));
	} else {
		char	_sig [11];

		memcpy (_sig, sig, 10);
		_sig [10] = 0;
 		ped_exception_throw (PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
			_("Unrecognised old style linux swap signature '%10s'."), _sig);
 		goto error_free_fs;
	}

	fs->checked = 1;
	return fs;

error_free_fs:
	swap_free (fs);
error:
	return NULL;
}

static PedFileSystem*
_swap_v1_open (PedGeometry* geom)
{
	PedFileSystem*		fs;
	SwapSpecific*		fs_info;
	const char*		sig;

	fs = swap_alloc (geom);
	if (!fs)
		goto error;
/* 	swap_init (fs, 0); */

/* 	fs_info = SWAP_SPECIFIC (fs); */
/* 	if (!ped_geometry_read (fs->geom, fs_info->header, 0, */
/* 				fs_info->page_sectors)) */
        if (!swap_init(fs, 0))
		goto error_free_fs;

        fs_info = SWAP_SPECIFIC (fs);

	sig = ((char*) fs_info->header) + getpagesize() - 10;
	if (strncmp (sig, "SWAPSPACE2", 10) == 0) {
		fs_info->version = 1;
		fs_info->page_count = fs_info->header->new.last_page;
	} else {
		char	_sig [11];

		memcpy (_sig, sig, 10);
		_sig [10] = 0;
 		ped_exception_throw (PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
			_("Unrecognised new style linux swap signature '%10s'."), _sig);
 		goto error_free_fs;
	}

	fs->checked = 1;
	return fs;

error_free_fs:
	swap_free (fs);
error:
	return NULL;
}

static PedFileSystem*
_swap_swsusp_open (PedGeometry* geom)
{
	PedFileSystem*		fs;
	SwapSpecific*		fs_info;
	const char*		sig;

	fs = swap_alloc (geom);
	if (!fs)
		goto error;
        fs->type = &_swap_swsusp_type;
	swap_init (fs, 0);

	fs_info = SWAP_SPECIFIC (fs);
	if (!ped_geometry_read (fs->geom, fs_info->header, 0,
				fs_info->page_sectors))
		goto error_free_fs;

	sig = ((char*) fs_info->header) + getpagesize() - 10;
       	if (strncmp (sig, "S1SUSPEND", 9) == 0) {
	        fs_info->version = -1;
	} else {
		char	_sig [10];

		memcpy (_sig, sig, 9);
		_sig [9] = 0;
 		ped_exception_throw (PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
			_("Unrecognised swsusp linux swap signature '%9s'."), _sig);
 		goto error_free_fs;
	}

	fs->checked = 1;
	return fs;

error_free_fs:
	swap_free (fs);
error:
	return NULL;
}

static int
swap_close (PedFileSystem* fs)
{
	swap_free (fs);
	return 1;
}

#ifndef DISCOVER_ONLY
static int
swap_new_find_bad_page (PedFileSystem* fs, unsigned int page)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	unsigned int	i;

	for (i=0; i < fs_info->header->new.nr_badpages; i++) {
		if (fs_info->header->new.badpages [i] == page)
			return i;
	}

	return 0;
}

static int
swap_new_remove_bad_page (PedFileSystem* fs, unsigned int page)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	unsigned int	pos;

	pos = swap_new_find_bad_page (fs, page);
	if (!pos)
		return 0;

	for (; pos < fs_info->header->new.nr_badpages; pos++) {
		fs_info->header->new.badpages [pos - 1]
			= fs_info->header->new.badpages [pos];
	}

	return 1;
}

static int
swap_mark_page (PedFileSystem* fs, unsigned int page, int ok)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	char*		ptr;
	unsigned int	mask;

	if (fs_info->version == 0) {
		ptr = &fs_info->header->old.page_map [page/8];
		mask = 1 << (page%8);
		*ptr = (*ptr & ~mask) + ok * mask;
	} else {
		if (ok) {
			if (swap_new_remove_bad_page (fs, page))
				fs_info->header->new.nr_badpages--;
		} else {
			if (swap_new_find_bad_page (fs, page))
				return 1;

			if (fs_info->header->new.nr_badpages
					> fs_info->max_bad_pages) {
				ped_exception_throw (PED_EXCEPTION_ERROR,
					PED_EXCEPTION_CANCEL,
					_("Too many bad pages."));
				return 0;
			}

			fs_info->header->new.badpages
				[fs_info->header->new.nr_badpages] = page;
			fs_info->header->new.nr_badpages++;
		}
	}

	return 1;
}

static void
swap_clear_pages (PedFileSystem* fs)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	unsigned int	i;

	for (i = 1; i < fs_info->page_count; i++) {
		swap_mark_page (fs, i, 1);
	}

	if (fs_info->version == 0) {
		for (; i < 1024; i++) {
			swap_mark_page (fs, i, 0);
		}
	}
}

static int
swap_check_pages (PedFileSystem* fs, PedTimer* timer)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	PedSector	result;
	int		first_page = 1;
	int		stop_page = 0;
	int		last_page = fs_info->page_count - 1;
	PedTimer*	nested_timer;

	ped_timer_reset (timer);
	ped_timer_set_state_name (timer, _("checking for bad blocks"));

	swap_clear_pages (fs);
	while (first_page <= last_page) {
		nested_timer = ped_timer_new_nested (
				  timer,
			       	  1.0 * (last_page - first_page) / last_page);
		result = ped_geometry_check (
				fs->geom,
				fs_info->buffer,
				fs_info->buffer_size / 512,
				first_page * fs_info->page_sectors,
				fs_info->page_sectors,
				(last_page - first_page + 1)
					* fs_info->page_sectors,
				nested_timer);
		ped_timer_destroy_nested (nested_timer);
		if (!result)
			return 1;
		stop_page = result / fs_info->page_sectors;
		if (!swap_mark_page (fs, stop_page, 0))
			return 0;
		first_page = stop_page + 1;
	}
	return 1;
}

static int
swap_write (PedFileSystem* fs)
{
	SwapSpecific*	fs_info = SWAP_SPECIFIC (fs);
	char*		sig = ((char*) fs_info->header) + getpagesize() - 10;

	if (fs_info->version == 0) {
		memcpy (sig, "SWAP-SPACE", 10);
	} else {
		fs_info->header->new.version = 1;
		fs_info->header->new.last_page = fs_info->page_count - 1;
		fs_info->header->new.nr_badpages = 0;
		memcpy (sig, "SWAPSPACE2", 10);
	}

	return ped_geometry_write (fs->geom, fs_info->header, 0,
				   fs_info->page_sectors);
}

static PedFileSystem*
swap_create (PedGeometry* geom, PedTimer* timer)
{
	PedFileSystem*		fs;

	fs = swap_alloc (geom);
	if (!fs)
		goto error;
	swap_init (fs, 1);
	if (!swap_write (fs))
		goto error_free_fs;
	return fs;

error_free_fs:
	swap_free (fs);
error:
	return NULL;
}

static int
swap_resize (PedFileSystem* fs, PedGeometry* geom, PedTimer* timer)
{
	PedGeometry*	old_geom = fs->geom;

	fs->geom = ped_geometry_duplicate (geom);
	swap_init (fs, old_geom->start != geom->start);
	if (!swap_write (fs))
		goto error;
	ped_geometry_destroy (old_geom);
	return 1;

error:
	ped_geometry_destroy (fs->geom);
	fs->geom = old_geom;
	return 0;
}

static PedFileSystem*
swap_copy (const PedFileSystem* fs, PedGeometry* geom, PedTimer* timer)
{
	return ped_file_system_create (geom, &_swap_v1_type, timer);
}

static int
swap_check (PedFileSystem* fs, PedTimer* timer)
{
	return swap_check_pages (fs, timer)
		&& swap_write (fs);
}

static PedConstraint*
swap_get_create_constraint (const PedDevice* dev)
{
	PedGeometry	full_dev;

	if (!ped_geometry_init (&full_dev, dev, 0, dev->length - 1))
		return NULL;

	return ped_constraint_new (ped_alignment_any, ped_alignment_any,
				   &full_dev, &full_dev,
				   getpagesize() / 512, dev->length);
}

static PedConstraint*
swap_get_resize_constraint (const PedFileSystem* fs)
{
	return swap_get_create_constraint (fs->geom->dev);
}

static PedConstraint*
swap_get_copy_constraint (const PedFileSystem* fs, const PedDevice* dev)
{
	return swap_get_create_constraint (dev);
}
#endif /* !DISCOVER_ONLY */

static PedGeometry*
_swap_v0_probe (PedGeometry* geom) {
        return _generic_swap_probe (geom, 0);
}

static PedGeometry*
_swap_v1_probe (PedGeometry* geom) {
        return _generic_swap_probe (geom, 1);
}

static PedGeometry*
_swap_swsusp_probe (PedGeometry* geom) {
        return _generic_swap_probe (geom, -1);
}

static int
_swap_v0_clobber (PedGeometry* geom) {
        return _generic_swap_clobber (geom, 0);
}

static int
_swap_v1_clobber (PedGeometry* geom) {
        return _generic_swap_clobber (geom, 1);
}

static int
_swap_swsusp_clobber (PedGeometry* geom) {
        return _generic_swap_clobber (geom, -1);
}

static PedFileSystemOps _swap_v0_ops = {
	probe:		_swap_v0_probe,
#ifndef DISCOVER_ONLY
	clobber:	_swap_v0_clobber,
	open:		_swap_v0_open,
	create:		swap_create,
	close:		swap_close,
	check:		swap_check,
	copy:		swap_copy,
	resize:		swap_resize,
	get_create_constraint:	swap_get_create_constraint,
	get_resize_constraint:	swap_get_resize_constraint,
	get_copy_constraint:	swap_get_copy_constraint
#else
	clobber:	NULL,
	open:		NULL,
	create:		NULL,
	close:		NULL,
	check:		NULL,
	copy:		NULL,
	resize:		NULL,
	get_create_constraint:	NULL,
	get_resize_constraint:	NULL,
	get_copy_constraint:	NULL
#endif /* !DISCOVER_ONLY */
};

static PedFileSystemOps _swap_v1_ops = {
	probe:		_swap_v1_probe,
#ifndef DISCOVER_ONLY
	clobber:	_swap_v1_clobber,
	open:		_swap_v1_open,
	create:		swap_create,
	close:		swap_close,
	check:		swap_check,
	copy:		swap_copy,
	resize:		swap_resize,
	get_create_constraint:	swap_get_create_constraint,
	get_resize_constraint:	swap_get_resize_constraint,
	get_copy_constraint:	swap_get_copy_constraint
#else
	clobber:	NULL,
	open:		NULL,
	create:		NULL,
	close:		NULL,
	check:		NULL,
	copy:		NULL,
	resize:		NULL,
	get_create_constraint:	NULL,
	get_resize_constraint:	NULL,
	get_copy_constraint:	NULL
#endif /* !DISCOVER_ONLY */
};

static PedFileSystemOps _swap_swsusp_ops = {
  probe:		_swap_swsusp_probe,
#ifndef DISCOVER_ONLY
	clobber:	_swap_swsusp_clobber,
	open:		_swap_swsusp_open,
	create:		swap_create,
	close:		swap_close,
	check:		swap_check,
	copy:		swap_copy,
	resize:		swap_resize,
	get_create_constraint:	swap_get_create_constraint,
	get_resize_constraint:	swap_get_resize_constraint,
	get_copy_constraint:	swap_get_copy_constraint
#else
	clobber:	NULL,
	open:		NULL,
	create:		NULL,
	close:		NULL,
	check:		NULL,
	copy:		NULL,
	resize:		NULL,
	get_create_constraint:	NULL,
	get_resize_constraint:	NULL,
	get_copy_constraint:	NULL
#endif /* !DISCOVER_ONLY */
};

static PedFileSystemType _swap_v0_type = {
	next:	NULL,
	ops:	&_swap_v0_ops,
	name:	"linux-swap(v0)",
	block_sizes: LINUXSWAP_BLOCK_SIZES
};

static PedFileSystemType _swap_v1_type = {
	next:	NULL,
	ops:	&_swap_v1_ops,
	name:	"linux-swap(v1)",
	block_sizes: LINUXSWAP_BLOCK_SIZES
};

static PedFileSystemType _swap_swsusp_type = {
        next:   NULL,
	ops:    &_swap_swsusp_ops,
	name:   "swsusp",
        block_sizes: LINUXSWAP_BLOCK_SIZES
};

void
ped_file_system_linux_swap_init ()
{
	ped_file_system_type_register (&_swap_v0_type);
	ped_file_system_type_register (&_swap_v1_type);
	ped_file_system_type_register (&_swap_swsusp_type);

	ped_file_system_alias_register (&_swap_v0_type, "linux-swap(old)", 1);
	ped_file_system_alias_register (&_swap_v1_type, "linux-swap(new)", 1);
	ped_file_system_alias_register (&_swap_v1_type, "linux-swap", 0);
}

void
ped_file_system_linux_swap_done ()
{
	ped_file_system_alias_unregister (&_swap_v0_type, "linux-swap(old)");
	ped_file_system_alias_unregister (&_swap_v1_type, "linux-swap(new)");
	ped_file_system_alias_unregister (&_swap_v1_type, "linux-swap");

	ped_file_system_type_unregister (&_swap_v0_type);
	ped_file_system_type_unregister (&_swap_v1_type);
	ped_file_system_type_unregister (&_swap_swsusp_type);
}
