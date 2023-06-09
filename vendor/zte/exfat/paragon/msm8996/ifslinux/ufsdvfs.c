// <copyright file="ufsdvfs.c" company="Paragon Software Group">
// EXCEPT WHERE OTHERWISE STATED, THE INFORMATION AND SOURCE CODE CONTAINED
// HEREIN AND IN RELATED FILES IS THE EXCLUSIVE PROPERTY OF PARAGON SOFTWARE
// GROUP COMPANY AND MAY NOT BE EXAMINED, DISTRIBUTED, DISCLOSED, OR REPRODUCED
// IN WHOLE OR IN PART WITHOUT EXPLICIT WRITTEN AUTHORIZATION FROM THE COMPANY.
//
// Copyright (c) 1994-2017 Paragon Software Group, All rights reserved.
//
// UNLESS OTHERWISE AGREED IN A WRITING SIGNED BY THE PARTIES, THIS SOFTWARE IS
// PROVIDED "AS-IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE, ALL OF WHICH ARE HEREBY DISCLAIMED. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
// </copyright>
/*++

Module Name:

    ufsdvfs.c

Abstract:

    This module implements VFS entry points for
    UFSD-based Linux filesystem driver.

Author:

    Ahdrey Shedel

Revision History:

    27/12/2002 - Andrey Shedel - Created

    Since 29/07/2005 - Alexander Mamaev

--*/

//
// This field is updated by SVN
//
const char s_FileVer[] = "$Id: ufsdvfs.c 315178 2017-11-08 11:26:58Z shura $";

//
// Tune ufsdvfs.c
//

//#define UFSD_COUNT_CONTAINED        "Use unix semantics for dir->i_nlink"
//#define UFSD_BUILTINT_UTF8          "Use builtin utf8 code page"
#ifdef UFSD_BUILTINT_UTF8
#pragma message "Use builtin utf8 code page"
#endif
#ifdef UFSD_DEBUG
#define UFSD_DEBUG_ALLOC            "Track memory allocation/deallocation"
#endif

//#define UFSD_CHECK_TIME        "Activate this define if you want to check the execution time of some functions (Debug only)"
//#define UFSD_USE_BH            "Force to use bh for each page"
#define UFSD_CLOSE_AT_RELEASE  "Close ufsd objects at release. Experimental feature (ntfs & hfs only)"
//#define UFSD_USE_BD_ZERO       "Force to zero range of file with ufsd_bd_zero"

// Activate this define to test readdir
//#define UFSD_EMULATE_SMALL_READDIR_BUFFER 10

#ifndef UFSD_SMART_DIRTY_SEC
  #define UFSD_SMART_DIRTY_SEC  5
#endif

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/nls.h>
#include <linux/uaccess.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <linux/statfs.h>
#include <linux/vermagic.h>
#include <linux/mpage.h>
#include <linux/blkdev.h>
#include <linux/delay.h> // jiffies_to_msecs
#include <linux/fs_struct.h>
#include <linux/aio.h>
#include <linux/pagevec.h>
#include <linux/namei.h>
#include <linux/swap.h>
#include <linux/bit_spinlock.h>
#include <linux/prefetch.h>
#include <linux/exportfs.h>
#include <linux/mutex.h>
#include <linux/ratelimit.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/ctype.h> // tolower
#include <asm/div64.h> // this file defines macros 'do_div'

#include "config.h"
#include "ufsdapi.h"
#include "vfsdebug.h"

#ifdef WRITE_SYNC
  #define clean_bdev_aliases(bdev,devblock,count) unmap_underlying_metadata(bdev,devblock)
#endif

#ifdef CONFIG_DEBUG_MUTEXES
//#warning "CONFIG_DEBUG_MUTEXES is ON"
#pragma message "CONFIG_DEBUG_MUTEXES is ON"
#endif

// Unfortunately #pragma message uses stderr to print the message
#pragma message "PAGE_SHIFT=" __stringify(PAGE_SHIFT)
#pragma message "THREAD_SIZE=" __stringify(THREAD_SIZE)

#if defined UFSD_REFS_ONLY && THREAD_SIZE < 16*1024
  #error "Refs requires 16K+ stack"
#endif

#if defined HAVE_LINUX_SCHED_MM_H && HAVE_LINUX_SCHED_MM_H
  #include <linux/sched/mm.h>
#endif

#if defined HAVE_LINUX_UIDGID_H && HAVE_LINUX_UIDGID_H
  #include <linux/uidgid.h>
#else
  #define __kuid_val( x )  (x)
  #define __kgid_val( x )  (x)
  #define uid_eq( v1, v2 ) ( v1 == v2 )
  #define gid_eq( v1, v2 ) ( v1 == v2 )
  #define KUIDT_INIT(value) ( value )
  #define KGIDT_INIT(value) ( value )
  #define GLOBAL_ROOT_UID  0
  #define GLOBAL_ROOT_GID  0
#endif

#if is_struct( BIO_BI_ITER )
  #define BIO_BISECTOR( bio ) (bio)->bi_iter.bi_sector
  #define BIO_BISIZE( bio )   (bio)->bi_iter.bi_size
#else
  #define BIO_BISECTOR( bio ) (bio)->bi_sector
  #define BIO_BISIZE( bio )   (bio)->bi_size
#endif

#ifdef BIO_UPTODATE
  #define BIO_RESULT( bio ) ({  \
    if (error)  \
      clear_bit(BIO_UPTODATE, &bio->bi_flags);  \
    else if (!test_bit(BIO_UPTODATE, &bio->bi_flags)) \
      error = -EIO; \
    error;  \
})
#elif defined HAVE_STRUCT_BIO_BI_ERROR
  // 4.3+
  #define BIO_RESULT( bio ) (bio)->bi_error
#elif defined HAVE_STRUCT_BIO_BI_STATUS
  // 4.13+
  #define BIO_RESULT( bio ) (bio)->bi_status
#endif

#if defined CONFIG_FS_POSIX_ACL
  #include <linux/posix_acl_xattr.h>

  #if is_decl( MODE_TYPE_MODE_T )
    #define posix_acl_mode mode_t
  #elif is_decl( MODE_TYPE_UMODE_T )
    #define posix_acl_mode umode_t
  #endif
#endif

#ifndef AOP_FLAG_UNINTERRUPTIBLE
  #define AOP_FLAG_UNINTERRUPTIBLE 0
#endif

#if defined UFSD_HFS && !defined DCACHE_DIRECTORY_TYPE
  // Hfs fork support ( - 3.12]
  #define UFSD_USE_HFS_FORK "Hfs fork support included"
#endif

//
// Default trace level for many functions in this module
//
#define Dbg  UFSD_LEVEL_VFS

#define UFSD_PACKAGE_STAMP ", " __DATE__" "__TIME__

//
// driver features
//
const char s_DriverVer[] = PACKAGE_VERSION
#ifdef PACKAGE_TAG
   " " PACKAGE_TAG
#else
   UFSD_PACKAGE_STAMP
#endif
#ifdef UFSD_BUILD_HOST
  ", paragon"
#endif
#if !defined __LP64__ && !defined CONFIG_LBD && !defined CONFIG_LBDAF
  ", LBD=OFF"
#endif
#if defined CONFIG_FS_POSIX_ACL
  ", acl"
#endif
#if !defined UFSD_NO_USE_IOCTL
  ", ioctl"
#endif
  ", sd2(" __stringify(UFSD_SMART_DIRTY_SEC) ")"
#ifdef UFSD_DEBUG
  ", debug"
#elif defined UFSD_TRACE
  ", tr"
#endif
#ifdef CONFIG_DEBUG_MUTEXES
  ", dm"
#endif
#ifdef UFSD_USE_HFS_FORK
  ", rsrc"
#endif
#ifdef UFSD_USE_BH
  ", bh"
#endif
#if ( defined UFSD_NTFS || defined UFSD_HFS ) && defined UFSD_CLOSE_AT_RELEASE
  ", car"
#endif
  ;

#if (defined CONFIG_NLS | defined CONFIG_NLS_MODULE) & !defined UFSD_BUILTINT_UTF8
  #define UFSD_USE_NLS  "Use nls functions instead of builtin utf8 to convert strings"
#endif

//
// Implement missing functions and helper macroses to reduce chaos
//
#if !( is_decl( BLK_START_PLUG ) )
  struct blk_plug{};
  static inline void blk_start_plug( struct blk_plug *plug ){}
  static inline void blk_finish_plug( struct blk_plug *plug ){}
#endif

#if !defined UFSD_AVOID_COPY_PAGE && is_decl( COPY_PAGE ) && !defined UFSD_FAT
  #define CopyPage( a, b ) copy_page( (a), (b) )
#endif

#if !( is_decl( FILE_INODE ) )
  #define file_inode(x) (x)->f_dentry->d_inode
#endif

#if is_struct( FILE_F_DENTRY )
  #define file_dentry(__f) ((__f)->f_dentry)
#else
  #define file_dentry(__f) ((__f)->f_path.dentry)
#endif

#ifndef IS_DAX
  #define IS_DAX(i) 0
#endif

#ifdef UFSD_TRACE
  #define lock_ufsd(s)     _lock_ufsd( s, __func__ )
  #define try_lock_ufsd(s)  _try_lock_ufsd( s, __func__ )
  #define unlock_ufsd(s)   _unlock_ufsd( s, __func__ )
  DEBUG_ONLY( static unsigned long WaitMutex; )
  static unsigned long StartJiffies;
#else
  #define lock_ufsd(s)     _lock_ufsd( s )
  #define try_lock_ufsd(s)  _try_lock_ufsd( s )
  #define unlock_ufsd(s)   _unlock_ufsd( s )
#endif

#ifdef UFSD_DEBUG
  #define ProfileEnter(s,name)    \
    spin_lock( &s->prof_lock );   \
    s->name##_cnt += 1;           \
    s->name##_ticks -= jiffies;   \
    spin_unlock( &s->prof_lock )

  #define ProfileLeave(s,name)    \
    spin_lock( &s->prof_lock );   \
    s->name##_ticks += jiffies;   \
    spin_unlock( &s->prof_lock )
#else
  #define ProfileEnter(s,name)
  #define ProfileLeave(s,name)
#endif

#if defined UFSD_DEBUG && defined UFSD_CHECK_TIME
  #define CheckTime(sec)          \
    j0 = jiffies - j0;            \
    if ( j0 > sec*HZ ) {          \
      ufsd_trace( "**** %s - %u", __func__, jiffies_to_msecs( j0 ) ); \
    }
  #define CheckTimeEx(sec,format,...) \
    j0 = jiffies - j0;            \
    if ( j0 > sec*HZ ) {          \
      ufsd_trace( "**** %s - %u, " format "", __func__, jiffies_to_msecs( j0 ), ## __VA_ARGS__ ); \
    }
  #define CHECK_TIME_ONLY(x) x
#else
  #undef  UFSD_CHECK_TIME
  #define CheckTime(sec)
  #define CheckTimeEx(sec,format,...)
  #define CHECK_TIME_ONLY(x)
#endif

#ifdef UFSD_CHECK_STACK
  #define current_sp() ({ void *sp; __asm__("movq %%rsp, %0" : "=r" (sp) : ); sp; })
  #define current_bp() ({ unsigned long bp; __asm__("movq %%rbp, %0" : "=r" (bp) : ); bp; })

  static void*    s_sp0;
  static ssize_t  s_dsp;
  static ssize_t  s_maxdsp;
  const char*     s_max_hint;
  const char*     s_hint0;

  #define ufsd_check_sp_start( h )  ({s_dsp = 0; s_sp0 = current_sp(); s_hint0 = (h);})

  void UFSDAPI_CALL ufsd_check_sp_() {
    ssize_t dsp = s_sp0 - current_sp();
    if ( dsp > s_dsp ) {
//      DebugTrace( 0, 0, ( "**** %s: %zd -> %zd\n", __func__, s_dsp, dsp ));
      s_dsp = dsp;
    }
    if ( dsp > s_maxdsp && dsp < 8*1024 ) {
      DebugTrace( 0, 0, ( "**** %s: %zd -> %zd\n", __func__, s_maxdsp, dsp ));
      s_maxdsp    = dsp;
      s_max_hint  = s_hint0;
      if ( dsp >= 6600 )//2*PAGE_SIZE )
        dump_stack();
    }
  }
#else
  #define ufsd_check_sp_start( h )
  #define ufsd_check_sp()
#endif

#if !( is_decl( INODE_OWNER_OR_CAPABLE ) )
  // 2.6.39--
  #define inode_owner_or_capable  is_owner_or_cap
#endif

#if !( is_decl( SET_NLINK ) )
static inline void set_nlink(struct inode *i, unsigned int nlink){ i->i_nlink = nlink; }
#endif
#if !( is_decl( DROP_NLINK ) )
static inline void drop_nlink(struct inode *i){ i->i_nlink--; }
#endif
#if !( is_decl( INC_NLINK ) )
static inline void inc_nlink(struct inode *i){ i->i_nlink++; }
#endif

#ifndef UFSD_MODULE_CORE
  #define UFSD_MODULE_CORE() (void*)0
#endif

#if is_decl( TRY_TO_WRITEBACK_INODES_SB )
  #define Try_to_writeback_inodes_sb(s) try_to_writeback_inodes_sb( (s), WB_REASON_FREE_MORE_MEM )
#elif is_decl( WRITEBACK_INODES_SB_IF_IDLE_V1 )
  #define Try_to_writeback_inodes_sb(s) writeback_inodes_sb_if_idle( (s) )
#elif is_decl( WRITEBACK_INODES_SB_IF_IDLE_V2 )
  #define Try_to_writeback_inodes_sb(s) writeback_inodes_sb_if_idle( (s), WB_REASON_FREE_MORE_MEM )
#endif

#if is_decl( GET_USER_PAGES_V1 ) // 4.5-, write, no force
  #define Get_user_pages( __strt, __nr, __pgs )  get_user_pages( current, current->mm, __strt, __nr, 1, 0, __pgs, 0 )
#elif is_decl( GET_USER_PAGES_V2 ) // 4.6 - 4.8, write, no force
  #define Get_user_pages( __strt, __nr, __pgs )  get_user_pages( __strt, __nr, 1, 0, __pgs, 0 )
#elif is_decl( GET_USER_PAGES_V3 ) // 4.9+, write, no force
  #define Get_user_pages( __strt, __nr, __pgs )  get_user_pages( __strt, __nr, FOLL_WRITE, __pgs, 0 )
#endif

#ifdef PAGE_CACHE_SHIFT
  C_ASSERT( PAGE_CACHE_SHIFT == PAGE_SHIFT );
#endif

#if !( is_decl( FILE_REMOVE_PRIVS ) )
  #define file_remove_privs file_remove_suid
#endif

#if !( is_decl( BIO_GET_NR_VECS ) )
  #define bio_get_nr_vecs(s) BIO_MAX_PAGES
#endif

#define LOG2OF_PAGES_PER_MB ( 20 - PAGE_SHIFT )

// Add missing define
#ifndef REQ_PRIO
  #define REQ_PRIO  0
#endif

#ifndef REQ_META
  #define REQ_META  0
#endif

//
// This function returns UFSD's handle for 'inode'
//
// ufsd_file* UFSD_FH( IN struct inode *inode );
//
#define UFSD_FH(i)      (UFSD_U(i)->ufile)

#define UFSD_SB(sb)     ((usuper*)(sb)->s_fs_info)
#define UFSD_VOLUME(sb) UFSD_SB(sb)->ufsd

#define UFSD_SBI_FLAGS_ABORTED    0x00000001
#define UFSD_SBI_FLAGS_DISRCARD   0x00000002

//
// This function returns 'unode' for 'inode'
//
// struct unode* UFSD_U( IN struct inode* inode );
//
#define UFSD_U(inode)   (container_of((inode), struct unode, i))

//
// Private superblock structure.
// Stored in super_block.s_fs_info
//
typedef struct usuper {
    struct super_block *sb;             // sometimes usefull
    UINT64            dev_size;         // size of block device in bytes
    UINT64            max_block;        // size of block device in blocks
    UINT64            end_of_dir;         // End of directory
    ufsd_volume       *ufsd;
#if !is_struct( FILE_SYSTEM_TYPE_MOUNT )
    struct vfsmount   *vfs_mnt;
    char              mnt_buffer[32];
#endif
    unsigned long     flags;              // UFSD_SBI_FLAGS_XXX ...
    struct mutex      api_mutex;
    spinlock_t        nocase_lock;
    mount_options     options;
    struct backing_dev_info *bdi;         // bdi on mount. Used to check for surprise remove
    CHECK_TIME_ONLY( unsigned long lock_time; )
#ifdef CONFIG_FS_POSIX_ACL
    void              *x_buffer;
    size_t            bytes_per_xbuffer;
#endif

#if UFSD_SMART_DIRTY_SEC
    rwlock_t            state_lock;        // Protect the various scalars
    wait_queue_head_t   wait_done_flush;
    wait_queue_head_t   wait_exit_flush;
    struct task_struct  *flush_task;       // Pointer to the current flush thread for this volume
    struct timer_list   flush_timer;       // The timer used to wakeup the flush thread
    unsigned char       exit_flush_timer;  // Used to exit from flush thread
    unsigned long       last_dirty;
#endif
    unsigned char       bdirty;

#if defined CONFIG_PROC_FS
    struct proc_dir_entry *procdir;
#endif
    TRACE_ONLY( struct sysinfo    sys_info; ) // to save stack
    spinlock_t        ddt_lock;           // do_delayed_tasks lock
    struct list_head  clear_list;         // List of inodes to clear
    struct list_head  write_list;         // List of inodes to write

#ifdef UFSD_NTFS
    #define RW_BUFFER_SIZE  (4*PAGE_SIZE)
    void              *rw_buffer;         // RW_BUFFER_SIZE
    UINT64            maxbytes;           // Maximum size for normal files
#endif

    UINT64            cluster_mask_inv;   // ~(bytes_per_cluster-1)
    unsigned int      cluster_mask;       // bytes_per_cluster-1
    unsigned int      bytes_per_cluster;
    unsigned int      discard_granularity;
    UINT64            discard_granularity_mask_inv; // ~(discard_granularity_mask_inv-1)

    finfo*            fi;

#ifdef UFSD_DEBUG
    int               eject;             // emulate ejected
    size_t            nDelClear;
    size_t            nDelWrite;
    size_t            nWrittenBlocks;
    size_t            nReadBlocks;
    size_t            nWrittenBlocksNa;
    size_t            nReadBlocksNa;
    size_t            nMappedBh;
    size_t            nUnMappedBh;
    size_t            nPeakMappedBh;
    size_t            nHashCalls;
    size_t            nHashCallsUfsd;
    size_t            nCompareCalls;
    size_t            nCompareCallsUfsd;

    spinlock_t        prof_lock;      // protect below members

    // Internal profiler
    size_t            bdread_cnt;
    size_t            bdread_ticks;
    size_t            bdwrite_cnt;
    size_t            bdwrite_ticks;
    size_t            bdflush_cnt;
    size_t            bdflush_ticks;
    size_t            bdmap_cnt;
    size_t            bdmap_ticks;
    size_t            bdsetdirty_cnt;
    size_t            bdsetdirty_ticks;
    size_t            bdunmap_meta_cnt;
    size_t            bdunmap_meta_ticks;
    size_t            bdunmap_meta_sync;
    size_t            bd_discard_cnt;
    size_t            bd_discard_ticks;
    size_t            bd_zero_cnt;
    size_t            bd_zero_ticks;
    size_t            write_begin_cnt;
    size_t            write_begin_ticks;
    size_t            write_end_cnt;
    size_t            write_end_ticks;
    size_t            write_inode_cnt;
    size_t            write_inode_ticks;
    size_t            readpage_cnt;
    size_t            readpage_ticks;
    size_t            readpages_cnt;
    size_t            readpages_ticks;
    size_t            do_readpage_cnt;
    size_t            do_readpage_ticks;
    size_t            buf_readpage_cnt;
    size_t            buf_readpage_ticks;
    size_t            writepage_cnt;
    size_t            writepage_ticks;
    size_t            writepages_cnt;
    size_t            writepages_ticks;
    size_t            do_writepage_cnt;
    size_t            do_writepage_ticks;
    size_t            buf_writepage_cnt;
    size_t            buf_writepage_ticks;
    size_t            buf_get_cnt;
    size_t            buf_get_ticks;
    size_t            buf_put_cnt;
    size_t            buf_put_ticks;
    size_t            buf_write_cnt;
    size_t            buf_write_ticks;
#endif
#ifdef Try_to_writeback_inodes_sb
    atomic_t          writeiter_cnt;
    atomic_t          dirty_pages_count; // number of dirty pages
#endif
    atomic_t          VFlush;       // Need volume flush

} usuper;


#define UFSD_UNODE_FLAG_FORK_BIT        0   // inode is resource fork
#define UFSD_UNODE_FLAG_LAZY_INIT_BIT   2
#define UFSD_UNODE_FLAG_SPARSE_BIT      9   // <ufsd> file is sparsed
#define UFSD_UNODE_FLAG_COMPRESS_BIT    11  // <ufsd> file is compressed
#define UFSD_UNODE_FLAG_ENCRYPT_BIT     14  // <ufsd> file is encrypted
#define UFSD_UNODE_FLAG_STREAMS_BIT     28  // <ufsd> file contains streams
#define UFSD_UNODE_FLAG_EA_BIT          29  // <ufsd> file contains extended attributes
//#define UFSD_UNODE_FLAG_RESIDENT_BIT    30  // <ufsd> file is resident

#define UFSD_UNODE_FLAG_SPARSE    (1u<<UFSD_UNODE_FLAG_SPARSE_BIT)
#define UFSD_UNODE_FLAG_COMPRESS  (1u<<UFSD_UNODE_FLAG_COMPRESS_BIT)
#define UFSD_UNODE_FLAG_ENCRYPT   (1u<<UFSD_UNODE_FLAG_ENCRYPT_BIT)
#define UFSD_UNODE_FLAG_STREAMS   (1u<<UFSD_UNODE_FLAG_STREAMS_BIT)
#define UFSD_UNODE_FLAG_EA        (1u<<UFSD_UNODE_FLAG_EA_BIT)
//#define UFSD_UNODE_FLAG_RESIDENT  (1u<<UFSD_UNODE_FLAG_RESIDENT_BIT)

#define UFSD_UNODE_FLAG_API_FLAGS (UFSD_UNODE_FLAG_SPARSE | UFSD_UNODE_FLAG_COMPRESS | UFSD_UNODE_FLAG_ENCRYPT | UFSD_UNODE_FLAG_EA )//| UFSD_UNODE_FLAG_RESIDENT)

//
// In memory ufsd inode
//
typedef struct unode {
  rwlock_t      rwlock;

  //
  // 'init_once' initialize members [0 - 'ufile')
  // 'ufsd_alloc_inode' resets members ['ufile' - 'i')
  //
  ufsd_file     *ufile;

  // one saved fragment. protected by rwlock
  loff_t        vbo, lbo, len;
  // valid size. protected by rwlock
  loff_t        valid;

#ifdef UFSD_USE_HFS_FORK
  struct inode  *fork_inode;
#endif
  unsigned long flags;                // UFSD_UNODE_FLAG_XXX bits

#ifdef UFSD_NTFS
  loff_t        total_alloc;          // total allocated for sparse files
#endif

  atomic_t      write_begin_end_cnt;  // not paired write_begin counter

  // Flag and fields for storing of uid / gid / mode in "no access rules" mode
  char          stored_noacsr;
  umode_t       i_mode;
#if defined HAVE_LINUX_UIDGID_H && HAVE_LINUX_UIDGID_H
  kuid_t        i_uid;
  kgid_t        i_gid;
#else
  uid_t         i_uid;
  gid_t         i_gid;
#endif

  atomic_t      i_opencount;          // number of success opens

  //
  // vfs inode
  //
  struct inode  i;

} unode;


#ifdef UFSD_NTFS
  static inline int is_sparsed( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_SPARSE ); }
  static inline int is_compressed( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_COMPRESS ); }
  static inline int is_sparsed_or_compressed( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_SPARSE | UFSD_UNODE_FLAG_COMPRESS ); }
  static inline int is_encrypted( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_ENCRYPT ); }
  // -1 - sparse, -2 - resident, -3 - compressed, -4 - encrypted
  static inline int is_lbo_ok( IN const UINT64 lbo ) { return lbo < ((UINT64)-4); }
#else
  #define is_sparsed( u )  0
  #define is_compressed( u ) 0
  #define is_sparsed_or_compressed( u ) 0
  #define is_encrypted( u ) 0
  #define is_lbo_ok( lbo ) 1
#endif

#if defined UFSD_NTFS || defined UFSD_HFS
  static inline int is_xattr( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_EA ); }
#else
  #define is_xattr( u ) 0
#endif

#ifdef UFSD_USE_HFS_FORK
  static inline int is_fork( IN const unode *u ) { return FlagOn( u->flags, (1u<<UFSD_UNODE_FLAG_FORK_BIT ) ); }
#else
  #define is_fork(u)  0
#endif


///////////////////////////////////////////////////////////
// set_valid_size
//
// Helper function to set valid size
///////////////////////////////////////////////////////////
static inline void
set_valid_size( IN unode *u, IN loff_t valid )
{
  unsigned long flags;
  write_lock_irqsave( &u->rwlock, flags );
  u->valid = valid;
  write_unlock_irqrestore( &u->rwlock, flags );
}


///////////////////////////////////////////////////////////
// get_valid_size
//
// Helper function to get usefull info from inode
///////////////////////////////////////////////////////////
static inline loff_t
get_valid_size(
    IN  unode  *u,
    OUT loff_t *i_size,
    OUT loff_t *i_bytes
    )
{
  unsigned long flags;
  loff_t valid;
  read_lock_irqsave( &u->rwlock, flags );
  valid   = u->valid;
  if ( NULL != i_size )
    *i_size = i_size_read( &u->i );
  if ( NULL != i_bytes )
    *i_bytes = inode_get_bytes( &u->i );
  read_unlock_irqrestore( &u->rwlock, flags );
  return valid;
}


///////////////////////////////////////////////////////////
// is_bdi_ok
//
// Returns 0 if bdi is removed
///////////////////////////////////////////////////////////
static int
is_bdi_ok(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );
  if ( likely( sbi->bdi == sb->s_bdi ) )
    return 1;

  DEBUG_ONLY( if ( likely( !sbi->eject ) ) return 1; )

  printk( KERN_CRIT QUOTED_UFSD_DEVICE": media \"%s\" removed\n", sb->s_id );
  return 0;
}


// How many seconds since 1970 till 1980
#define Seconds1970To1980     0x12CEA600

#ifdef UFSD_USE_POSIX_TIME
///////////////////////////////////////////////////////////
// posix2kernel
//
// Converts posix time into timestamp
///////////////////////////////////////////////////////////
static inline void
posix2kernel(
    IN  UINT64  tm,
    OUT struct timespec *ts
    )
{
  union utimespec ut;
  ut.time64   = tm;
  ts->tv_sec  = ut.tv_sec;
  ts->tv_nsec = ut.tv_nsec;
}


///////////////////////////////////////////////////////////
// kernel2posix
//
// Converts timestamp to posix time
///////////////////////////////////////////////////////////
static inline UINT64
kernel2posix(
    IN const struct timespec *ts
    )
{
  union utimespec ut;
  ut.tv_sec  = ts->tv_sec;
  ut.tv_nsec = ts->tv_nsec;
//  DebugTrace( 0, Dbg, ("time: %lx+%lu => %llx\n", ts->tv_sec, ts->tv_nsec, ut.time64 ));
  return ut.time64;
}


///////////////////////////////////////////////////////////
// current_time_posix
//
// This function returns the number of seconds since 1970
///////////////////////////////////////////////////////////
UINT64 UFSDAPI_CALL
current_time_posix( void )
{
  struct timespec ts = current_kernel_time();
  return kernel2posix( &ts );
}
#else
  #define posix2kernel( tm, ts )
  #define kernel2posix( ts ) 0
#endif // #ifdef UFSD_USE_POSIX_TIME


#ifdef UFSD_USE_NT_TIME
#define _100ns2seconds        10000000UL
#define SecondsToStartOf1970  0x00000002B6109100ULL

///////////////////////////////////////////////////////////
// kernel2nt
//
// Converts timestamp into nt time
///////////////////////////////////////////////////////////
static inline UINT64
kernel2nt(
    IN const struct timespec *ts
    )
{
  // 10^7 units of 100 nanoseconds in one second
  return _100ns2seconds * ( ts->tv_sec + SecondsToStartOf1970 ) + ts->tv_nsec/100;
}


///////////////////////////////////////////////////////////
// nt2kernel
//
// Converts nt time into timestamp
///////////////////////////////////////////////////////////
static inline void
nt2kernel(
    IN  const UINT64    tm,
    OUT struct timespec *ts
    )
{
  UINT64 t    = tm - _100ns2seconds*SecondsToStartOf1970;
  // WARNING: do_div changes its first argument(!)
  ts->tv_nsec = do_div( t, _100ns2seconds ) * 100;
  ts->tv_sec  = t;
}


///////////////////////////////////////////////////////////
// current_time_nt(GMT)
//
// This function returns the number of 100 nanoseconds since 1601
///////////////////////////////////////////////////////////
UINT64 UFSDAPI_CALL
current_time_nt( void )
{
  struct timespec ts = current_kernel_time();
  return kernel2nt( &ts );
}

#else
  #define nt2kernel( tm, ts )
  #define kernel2nt( ts ) 0
#endif // #ifdef UFSD_USE_NT_TIME


///////////////////////////////////////////////////////////
// ufsd_inode_current_time
//
// Returns current time (to store in inode)
///////////////////////////////////////////////////////////
static inline struct timespec
ufsd_inode_current_time(
    IN usuper *sbi
    )
{
  struct timespec ts = current_kernel_time();
  if ( is_hfs( &sbi->options ) )
    ts.tv_nsec = 0;
  else if ( is_fat( &sbi->options ) ) {
    // round up 2 seconds
    if ( ts.tv_nsec )
      ts.tv_sec += 1;
    if ( ts.tv_sec & 1 )
      ts.tv_sec += 1;
    ts.tv_nsec = 0;
  } else if ( is_exfat( &sbi->options ) )
    ts.tv_nsec -= ts.tv_nsec % (NSEC_PER_SEC / 100);
  else
    ts.tv_nsec -= ts.tv_nsec % 100;

  return ts;
}


///////////////////////////////////////////////////////////
// ufsd_time_trunc
//
// Truncate time to a granularity
// Returns 1 if changed
///////////////////////////////////////////////////////////
static inline int
ufsd_time_trunc(
    IN usuper *sbi,
    IN const struct timespec *ts,
    IN OUT struct timespec *td
    )
{
  struct timespec t;

  t.tv_sec  = ts->tv_sec;

  // timespec_trunc
  if ( is_hfs( &sbi->options ) )
    t.tv_nsec = 0;
  else if ( is_fat( &sbi->options ) ) {
    // round up 2 seconds
    if ( ts->tv_nsec )
      t.tv_sec += 1;
    if ( t.tv_sec & 1 )
      t.tv_sec += 1;
    t.tv_nsec = 0;
  } else if ( is_exfat( &sbi->options ) )
    t.tv_nsec = ts->tv_nsec - (ts->tv_nsec % (NSEC_PER_SEC / 100));
  else
    t.tv_nsec = ts->tv_nsec - (ts->tv_nsec % 100);

  if ( t.tv_sec == td->tv_sec && t.tv_nsec == td->tv_nsec )
    return 0;

//  DebugTrace( 0, Dbg, ("%lx+%lu, %lx+%lu -> %lx+%lu\n", ts->tv_sec, ts->tv_nsec, td->tv_sec, td->tv_nsec, t.tv_sec, t.tv_nsec ));

  td->tv_sec  = t.tv_sec;
  td->tv_nsec = t.tv_nsec;
  return 1;
}


///////////////////////////////////////////////////////////
// kernel2ufsd
//
// Converts timestamp into ufsd time
///////////////////////////////////////////////////////////
static inline UINT64
kernel2ufsd(
    IN const usuper *sbi,
    IN const struct timespec *ts
    )
{
  return is_posixtime( &sbi->options )? kernel2posix( ts ) : kernel2nt( ts );
}


#if defined UFSD_EXFAT || defined UFSD_FAT
//
// This variable is used to get the bias
//
extern struct timezone sys_tz;

///////////////////////////////////////////////////////////
// ufsd_bias
//
// Returns minutes west of Greenwich
///////////////////////////////////////////////////////////
int UFSDAPI_CALL
ufsd_bias( void )
{
  return sys_tz.tz_minuteswest;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_times_to_inode
//
// assume ufsd is locked  (fi is a pointer inside ufsd)
///////////////////////////////////////////////////////////
static inline void
ufsd_times_to_inode(
    IN const usuper   *sbi,
    IN const finfo    *fi,
    OUT struct inode  *i
    )
{
  if ( is_posixtime( &sbi->options ) ) {
    posix2kernel( fi->ReffTime  , &i->i_atime );
    posix2kernel( fi->ChangeTime, &i->i_ctime );
    posix2kernel( fi->ModiffTime, &i->i_mtime );
  } else {
    nt2kernel( fi->ReffTime  , &i->i_atime );
    nt2kernel( fi->ChangeTime, &i->i_ctime );
    nt2kernel( fi->ModiffTime, &i->i_mtime );
  }
//  DebugTrace( 0, Dbg, ("ufsd_times_to_inode: a=%lx+%lu, m=%lx+%lu\n", i->i_atime.tv_sec, i->i_atime.tv_nsec, i->i_mtime.tv_sec, i->i_mtime.tv_nsec ));
}


//
// Defines for "no access rules" mode
//
// 0777 mode
#define UFSD_NOACSR_MODE 0777
// group of permission attributes: ATTR_UID, ATTR_GID, ATTR_MODE
#define UFSD_NOACSR_ATTRS (ATTR_UID | ATTR_GID | ATTR_MODE)


///////////////////////////////////////////////////////////
// ufsd_to_linux
//
// Translates ufsd error codes into linux error codes
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_to_linux(
    IN int err // ufsd error
    )
{
  switch( err ) {
  case 0                  : return 0;
  case ERR_FSUNKNOWN      : return -EINVAL;       // -22
  case ERR_BADPARAMS      : return -EINVAL;       // -22
  case ERR_NOMEMORY       : return -ENOMEM;       // -12
  case ERR_NOFILEEXISTS   : return -ENOENT;       // -2
  case ERR_FILEEXISTS     : return -EEXIST;       // -17
  case ERR_NOSPC          : return -ENOSPC;       // -28
  case ERR_WPROTECT       : return -EROFS;        // -30
  case ERR_BADNAME_LEN    : return -ENAMETOOLONG; // -36
  case ERR_NOTIMPLEMENTED : return -EOPNOTSUPP;   // -95, -ENOSYS??
  case ERR_DIRNOTEMPTY    : return -ENOTEMPTY;    // -39
  case ERR_MAXIMUM_LINK   : return -EMLINK;       // -31
  case ERR_INSUFFICIENT_BUFFER: return -ENODATA;  // -61
  case ERR_MORE_DATA      : return -ERANGE;       // -34, -EOVERFLOW for ioctl
  case ERR_FILE_TOO_BIG   : return -EFBIG;        // -27
  }

  // -EIO and others
  if ( (int)err >= -500 && err <= 4096 )
    return err;

  // error -> err0r to avoid error detection in tests
  DebugTrace( 0, 0, ("**** %s: unknown ufsd err0r %x", current->comm, err ) );
  return -EINVAL; // -22
}


//
// Memory allocation routines.
// Debug version of memory allocation/deallocation routine performs
// detection of memory leak/overwrite
//
#ifdef UFSD_DEBUG_ALLOC

typedef struct memblock_head {
    struct list_head Link;
    unsigned int  asize;
    unsigned int  seq;
    unsigned int  size;
    unsigned char barrier[64 - 3*sizeof(int) - sizeof(struct list_head)];

  /*
     offset  0x40
     |---------------------|
     | Requested memory of |
     |   size 'DataSize'   |
     |---------------------|
  */
  //unsigned char barrier2[64 - 3*sizeof(int) - sizeof(struct list_head)];

} memblock_head;

typedef struct ufsd_mem_cache {
  struct kmem_cache *cache;
  char name[1];
} ufsd_mem_cache;

static size_t TotalKmallocs;
static size_t TotalVmallocs;
static size_t UsedMemMax;
static size_t TotalAllocs;
static size_t TotalAllocBlocks;
static size_t TotalAllocSequence;
static size_t MemMaxRequest;
static size_t MemMinRequest = -1;
static LIST_HEAD(TotalAllocHead);
static DEFINE_SPINLOCK( debug_mem_lock );


///////////////////////////////////////////////////////////
// trace_mem_report
//
// Helper function to trace memory usage information
///////////////////////////////////////////////////////////
static void
trace_mem_report(
    IN int OnExit
    )
{
  if ( -1 != MemMinRequest ) {
    size_t Mb = UsedMemMax/(1024*1024);
    size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
    size_t b  = UsedMemMax%1024;
    unsigned long level = OnExit? UFSD_LEVEL_ERROR : Dbg;

    if ( 0 != Mb ) {
      DebugTrace( 0, level, ("Memory report: Peak usage %zu.%03Zu Mb (%zu bytes), kmalloc %zu, vmalloc %zu",
                    Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
    } else {
      DebugTrace( 0, level, ("Memory report: Peak usage %zu.%03Zu Kb (%zu bytes),  kmalloc %zu, vmalloc %zu",
                    Kb, b, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
    }
    DebugTrace( 0, level, ("%s:  %zu bytes in %zu blocks, Min/Max requests: %zu/%zu bytes",
                  OnExit? "Leak":"Total allocated", TotalAllocs, TotalAllocBlocks, MemMinRequest, MemMaxRequest ) );
  }
}
#endif


///////////////////////////////////////////////////////////
// ufsd_heap_alloc
//
// memory allocation routine
// NOTE: __GFP_ZERO passed in kmalloc/vmalloc does not zero memory at least in kernels up to 2.6.23 (?)
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_heap_alloc(
    IN unsigned long size,
    IN int    zero
    )
{
#ifdef UFSD_DEBUG_ALLOC
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  memblock_head *head;
  int use_kmalloc;
  // Overhead includes private information and two barriers to check overwriting
  size_t asize = size + sizeof(memblock_head) + sizeof(head->barrier);

  if ( asize <= 2*PAGE_SIZE ) {
    use_kmalloc = 1;
    // size_t align
    asize = (asize + sizeof(size_t)-1) & ~(sizeof(size_t)-1);
    head  = kmalloc( asize, GFP_NOFS );
  } else {
    use_kmalloc = 0;
    asize = PAGE_ALIGN( asize );
    head  = __vmalloc( asize, GFP_NOFS, PAGE_KERNEL );
    assert( (size_t)head >= VMALLOC_START && (size_t)head < VMALLOC_END );
#ifdef UFSD_DEBUG
    if ( (size_t)head < VMALLOC_START || (size_t)head >= VMALLOC_END )
      ufsd_trace( "vmalloc(%zx) returns %p. Must be in range [%lx, %lx)", asize, head, (long)VMALLOC_START, (long)VMALLOC_END );
#endif
  }

  assert( NULL != head );
  if ( NULL == head ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("HeapAlloc(%lx) failed", size));
    return NULL;
  }
  assert( 0 == (asize & 1U) );

  // Fill head private fields
  head->asize = use_kmalloc? asize : (asize | 1);
  head->size  = size;

  //  assert( size > 2 ); // Activate to find small allocations

  //
  // fills two barriers to check memory overwriting
  //
  memset( &head->barrier[0], 0xde, sizeof(head->barrier) );
  if ( zero )
    memset( head + 1, 0, size );
  memset( Add2Ptr( head + 1, size), 0xed, asize - size - sizeof(memblock_head) );

  //
  // Insert allocated memory in global list and update statistics
  //
  spin_lock( &debug_mem_lock );
  list_add( &head->Link, &TotalAllocHead );
  if ( size > MemMaxRequest )
    MemMaxRequest = size;
  if ( size < MemMinRequest )
    MemMinRequest = size;
  head->seq   = ++TotalAllocSequence;
  use_kmalloc? ++TotalKmallocs : ++TotalVmallocs;
  TotalAllocs    += size;
  if( TotalAllocs > UsedMemMax )
    UsedMemMax = TotalAllocs;
  TotalAllocBlocks += 1;
  spin_unlock( &debug_mem_lock );

  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("alloc(%lx) -> %p%s, seq=%x", size, head+1, use_kmalloc? "" : "(v)", head->seq));

  CheckTime( 1 );
  ufsd_check_sp();
  return head + 1;
#else
  void *ptr;

  if ( size <= 2*PAGE_SIZE ) {
    ptr = kmalloc( size, zero?(GFP_NOFS|__GFP_ZERO) : GFP_NOFS );
  } else {
    ptr = __vmalloc( size, zero?(GFP_NOFS|__GFP_ZERO) : (GFP_NOFS), PAGE_KERNEL );
    assert( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END );
  }
  if ( NULL != ptr ) {
    DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("alloc(%lx) -> %p%s", size, ptr, size <= PAGE_SIZE?"" : "(v)" ));
    return ptr;
  }

  assert( !"no memory" );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("alloc(%lx) failed", size));
  return NULL;
#endif
}


///////////////////////////////////////////////////////////
// ufsd_heap_free
//
// memory deallocation routine
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_heap_free(
    IN void *p
    )
{
#ifdef UFSD_DEBUG_ALLOC
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  memblock_head *block;
  const unsigned char *tst;
  size_t o;
  const char* hint;

  if ( NULL == p )
    return;

#if 1
  // Fast but unsafe find
  block = (memblock_head*)p - 1;
#else
  // Safe but very slow. Use only if big trouble
  spin_lock( &debug_mem_lock );
  {
    struct list_head  *pos;
    block = NULL; // assume not found
    list_for_each( pos, &TotalAllocHead ){
      memblock_head *fnd = list_entry( pos, memblock_head, Link );
      if ( p == (void*)(fnd + 1) ) {
        block = fnd;
        break;
      }
    }
  }
  spin_unlock( &debug_mem_lock );

  if ( NULL == block ) {
    assert( !"failed to find block" );
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("HeapFree(%p) failed to find block", p ));
    return;
  }
#endif

  hint = NULL;
  // Verify head barrier
  for ( o = 0, tst = &block->barrier[0]; o < sizeof(block->barrier); o++ ) {
    if ( 0xde != tst[o] ) {
      hint = "head";
      break;
    }
  }

  if ( NULL == hint ) {
    size_t tsize = (block->asize & ~1) - block->size - sizeof(memblock_head);

    // Verify tail barrier
    for ( o = 0, tst = Add2Ptr( block + 1, block->size ); o < tsize; o++ ) {
      if ( 0xed != tst[o] ) {
        hint = "tail";
        break;
      }
    }
  }

  if ( NULL != hint ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** seq=%x: size 0x%x  asize 0x%x", block->seq, block->size, block->asize ));
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** HeapFree(%p) %s barrier failed at 0x%zx", p, hint, PtrOffset( block, tst ) + o ));
    ufsd_turn_on_trace_level();
    ufsdapi_dump_memory( block, 512 );
    ufsd_revert_trace_level();
    BUG_ON(1);
  }

  //
  // Remove allocated memory from global list and update statistics
  //
  spin_lock( &debug_mem_lock );
  list_del( &block->Link );
  TotalAllocs -= block->size;
  TotalAllocBlocks -= 1;
  spin_unlock( &debug_mem_lock );

  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("free(%p, %x) seq=%x", block + 1, block->size, block->seq));

  memset( block + 1, 0xcc, block->size );

  // declaration of vfree and kfree differs!
  if ( block->asize & 1U )
    vfree( block );
  else
    kfree( block );
  CheckTime( 1 );
  ufsd_check_sp();
#else
  if ( NULL != p ) {
    DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("HeapFree(%p)", p));
    if ( (size_t)p >= VMALLOC_START && (size_t)p < VMALLOC_END ) {
      // This memory was allocated via vmalloc
      vfree( p );
    } else {
      // This memory was allocated via kmalloc
      kfree( p );
    }
  }
#endif
}


#if defined UFSD_HFS || defined UFSD_EXFAT || defined UFSD_FAT || defined UFSD_NTFS || defined UFSD_REFS || defined UFSD_REFS3
///////////////////////////////////////////////////////////
// ufsd_cache_create
//
// Creates cache to allocate objects of the same size
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_cache_create(
    IN const char *Name,
    IN unsigned   size,
    IN unsigned   align
    )
{
  size_t name_size = strlen(Name) + 1;
  const char *real_name;
  char *buf;
  void* ret;

  if ( 0 != strcmp( QUOTED_UFSD_DEVICE, "ufsd" ) ) {
    name_size += sizeof(QUOTED_UFSD_DEVICE);

    buf = kmalloc( name_size, GFP_NOFS );
    if ( NULL == buf )
      return NULL;
    real_name = buf;

    snprintf( buf, name_size, "%s_%s", QUOTED_UFSD_DEVICE, Name );
  } else {
    buf       = NULL;
    real_name = Name;
  }

#ifdef UFSD_DEBUG_ALLOC
  {
    ufsd_mem_cache *umc = kmalloc( offsetof( ufsd_mem_cache, name ) + name_size, GFP_NOFS );
    if ( NULL != umc ) {
      memcpy( umc->name, real_name, name_size );

      umc->cache = kmem_cache_create( real_name, size, align, SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD, NULL );

      if ( NULL == umc->cache ) {
        kfree( umc );
        umc = NULL;
      }
      DebugTrace( 0, Dbg, ("cache_create(\"%s\", %x)", real_name, size ) );
    }
    ret = umc;
  }
#else
  ret = kmem_cache_create( real_name, size, align, SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD, NULL );
#endif

  if ( NULL != buf )
    kfree( buf );
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_cache_destroy
//
// Destroys cache
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_cache_destroy(
    IN void *Cache
    )
{
  struct kmem_cache* cache;
#ifdef UFSD_DEBUG_ALLOC
  ufsd_mem_cache *umc = Cache;
  cache = umc->cache;
  DebugTrace( 0, Dbg, ("cache_destroy(%s)", umc->name ) );
  kfree( umc );
#else
  cache = Cache;
#endif
  kmem_cache_destroy( cache );
}


///////////////////////////////////////////////////////////
// ufsd_cache_alloc
//
// Allocates memory from cache
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_cache_alloc(
    IN void *Cache,
    IN int  bZero
    )
{
  void  *p;
  struct kmem_cache* cache;
#ifdef UFSD_DEBUG_ALLOC
  ufsd_mem_cache *umc = Cache;
  cache = umc->cache;
#else
  cache = Cache;
#endif

  p = kmem_cache_alloc( cache, bZero? (__GFP_ZERO | GFP_NOFS ) : GFP_NOFS );
  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("cache(%s) -> %p", umc->name, p ) );
  return p;
}


///////////////////////////////////////////////////////////
// ufsd_cache_free
//
// Returns memory to cache
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_cache_free(
    IN void *Cache,
    IN void *p
    )
{
  struct kmem_cache* cache;
#ifdef UFSD_DEBUG_ALLOC
  ufsd_mem_cache *umc = Cache;
  cache = umc->cache;
#else
  cache = Cache;
#endif

  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("cache(%s) <- %p", umc->name, p ) );
  kmem_cache_free( cache, p );
}
#endif // #if defined UFSD_HFS || defined UFSD_EXFAT || defined UFSD_NTFS


#if defined UFSD_NTFS || defined UFSD_EXFAT || defined UFSD_REFS || defined UFSD_REFS3
#define UFSD_USED_SHARED_FUNCS
//
// Shared memory struct.
// Used to share memory between volumes
//
static DEFINE_SPINLOCK( s_shared_lock );

struct {
  void      *ptr;
  unsigned  len;
  int       cnt;
} s_shared[8];


///////////////////////////////////////////////////////////
// ufsd_set_shared
//
// Returns 'ptr' if pointer was saved in shared memory
// Returns not NULL if pointer was shared
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_set_shared(
    IN void     *ptr,
    IN unsigned bytes
    )
{
  void  *ret = NULL;
  int i, j = -1;

  spin_lock( &s_shared_lock );
  for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
    if ( 0 == s_shared[i].cnt )
      j = i;
    else if ( bytes == s_shared[i].len && 0 == memcmp ( s_shared[i].ptr, ptr, bytes ) ) {
      s_shared[i].cnt += 1;
      ret = s_shared[i].ptr;
      break;
    }
  }

  if ( NULL == ret && -1 != j ) {
    s_shared[j].ptr = ptr;
    s_shared[j].len = bytes;
    s_shared[j].cnt = 1;
    ret = ptr;
  }
  spin_unlock( &s_shared_lock );

  DebugTrace( 0, Dbg, ("set_shared(%p,%x) => %p",  ptr, bytes, ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_put_shared
//
// Returns 'ptr' if pointer is not shared anymore
// Returns NULL if pointer is still shared
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_put_shared(
    IN void *ptr
    )
{
  void  *ret = ptr;
  int i;

  spin_lock( &s_shared_lock );
  for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
    if ( 0 != s_shared[i].cnt && s_shared[i].ptr == ptr ) {
      if ( 0 != --s_shared[i].cnt )
        ret = NULL;
      break;
    }
  }
  spin_unlock( &s_shared_lock );

  DebugTrace( 0, Dbg, ("put_shared (%p) => %p",  ptr, ret ));
  return ret;
}
#endif // #if defined UFSD_NTFS || defined UFSD_EXFAT || defined UFSD_REFS || defined UFSD_REFS3


//
// NLS support routines requiring
// access to kernel-dependent nls_table structure.
//

///////////////////////////////////////////////////////////
// ufsd_char2uni
//
// Converts multibyte string to UNICODE string
// Returns the length of destination string in wide symbols
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_char2uni(
    OUT unsigned short      *ws,        // Destination UNICODE string
    IN  int                 max_out,    // Maximum UNICODE characters in ws
    IN  const unsigned char *s,         // Source BCS string
    IN  int                 len,        // The length of BCS strings in bytes
    IN  struct nls_table    *nls        // Code pages
    )
{
#ifdef UFSD_USE_NLS
  int ret   = 0;
  int len0  = len;

  for ( ;; ) {

    int charlen;
    wchar_t wc;

    if ( len <= 0 || 0 == *s )
      return ret; // The only correct way to exit

    if ( max_out <= 0 ) {
TooLittle:
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("A2U: too little output buffer" ) );
      return ret;
    }

    wc      = *ws;
    charlen = nls->char2uni( s, len, &wc );

    if ( charlen <= 0 ) {
      ufsd_printk( NULL, "%s failed to convert '%.*s' to unicode. Pos %d, chars %x %x %x",
                   nls->charset, len0, s - (len0-len), len0-len, (int)s[0], len > 1? (int)s[1] : 0, len > 2? (int)s[2] : 0 );
      //
      // Code one symbol
      //
      if ( max_out < 3 )
        goto TooLittle;

      *ws++ = '%';
      *ws++ = get_digit( *s >> 4 );
      *ws++ = get_digit( *s >> 0 );

      ret     += 3;
      max_out -= 3;
      len     -= 1;
      s       += 1;

    } else {
      assert( (unsigned short)wc == wc );

      *ws++    = (unsigned short)wc;
      ret     += 1;
      max_out -= 1;
      len     -= charlen;
      s       += charlen;
    }
  }

#else

  *ws = 0;
  return 0;

#endif
}


///////////////////////////////////////////////////////////
// ufsd_uni2char
//
// Converts UNICODE string to multibyte
// Returns the length of destination string in chars
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_uni2char(
    OUT unsigned char         *s,         // Destination BCS string
    IN  int                   max_out,    // Maximum bytes in BCS string
    IN  const unsigned short  *ws,        // Source UNICODE string
    IN  int                   len,        // The length of UNICODE string
    IN  struct nls_table      *nls        // Code pages
   )
{
#ifdef UFSD_USE_NLS
  unsigned char *s0 = s;

  for ( ;; ) {

    int charlen;

    if ( len <= 0 || 0 == *ws )
      return (int)(s - s0); // The only correct way to exit

    if ( max_out <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("U2A: too little output buffer" ) );
      return (int)(s - s0);
    }

    charlen = nls->uni2char( *ws, s, max_out );
    if ( charlen <= 0 ) {
      assert( !"U2A: failed to convert" );
      ufsd_printk( NULL, "%s failed to convert from unicode. pos %d, chars %x %x %x",
                   nls->charset, (int)(s-s0), (unsigned)ws[0], len > 1? (unsigned)ws[1] : 0, len > 2? (unsigned)ws[2] : 0 );
      return 0;
    }

    ws      += 1;
    len     -= 1;
    max_out -= charlen;
    s       += charlen;
  }

#else

  *s = 0;
  return 0;

#endif
}


#ifdef UFSD_FAT
///////////////////////////////////////////////////////////
// ufsd_toupper
//
// Converts UTF8 oe OEM string to upcase
// Returns 0 if ok
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_toupper(
    IN  struct nls_table      *nls,       // Code pages
    IN OUT unsigned char      *s,         // Source and Destination string
    IN  int                   len         // The length of ASCII or OEM string
    )
{
  int i;
  for ( i = 0; i < len; i++ )
    s[i] = nls_toupper( nls, s[i] );
}
#endif


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// ufsd_uload_nls
//
//
///////////////////////////////////////////////////////////
static void
ufsd_uload_nls(
    IN mount_options  *opts
    )
{
  int cp;
  for ( cp = 0; cp < opts->nls_count; cp++ ) {
    if ( NULL != opts->nls[cp] )
      unload_nls( opts->nls[cp] );
    opts->nls[cp] = NULL;
  }
  opts->nls_count = 0;

#ifdef UFSD_FAT
  if ( NULL != opts->nls_oem )
    unload_nls( opts->nls_oem );
#endif
}
#else
  #define ufsd_uload_nls( o )
#endif // #ifdef UFSD_USE_NLS


#ifdef REQ_OP_BITS
  // 4.8+
  #define Submit_bh                             submit_bh
  #define Submit_bio( op, op_flags, bio )       bio_set_op_attrs( bio, op, op_flags ); submit_bio( bio )
  #define Ll_rw_block                           ll_rw_block
#else
  #define Submit_bh( op, op_flags, bh )         submit_bh( op | op_flags, bh )
  #define Submit_bio( op, op_flags, bio )       submit_bio( op | op_flags, bio )
  #define Ll_rw_block( op, op_flags, nr, bh )   ll_rw_block( op | op_flags, nr, bh )
#endif


//
// Device IO functions.
//
#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// bh_tail
//
// Get buffer_head for tail
///////////////////////////////////////////////////////////
struct buffer_head*
bh_tail(
    IN struct super_block *sb,
    IN size_t              bytes2skip
    )
{
  struct buffer_head *bh;
  usuper *sbi = UFSD_SB( sb );
  sector_t TailBlock = ((sbi->max_block << sb->s_blocksize_bits) + bytes2skip) >> 9;
  struct page *page = alloc_page( GFP_NOFS | __GFP_ZERO );
  if ( NULL == page )
    return NULL;
  bh = alloc_buffer_head( GFP_NOFS );
  if ( NULL == bh ) {
out:
    __free_page( page );
    return NULL;
  }

  bh->b_state = 0;
  init_buffer( bh, end_buffer_read_sync, NULL );
  atomic_set( &bh->b_count, 2 );
  set_bh_page( bh, page, bytes2skip );
  bh->b_size    = 512;
  bh->b_bdev    = sb->s_bdev;
  bh->b_blocknr = TailBlock;
  set_buffer_mapped( bh );
  lock_buffer( bh );
  Submit_bh( READ, 0, bh );
  wait_on_buffer( bh );
  if ( !buffer_uptodate( bh ) ) {
    brelse( bh );
    goto out;
  }

  assert( 1 == atomic_read( &bh->b_count ) );
//    DebugTrace( 0, 0, ("bh_tail"));
  get_bh( bh );
  return bh;
}
#endif // #ifdef UFSD_HFS


#ifdef UFSD_TURN_OFF_READAHEAD
  #pragma message "turn off ufsd_bd_read_ahead"
#else

///////////////////////////////////////////////////////////
// ufsd_blkdev_get_block
//
// Default get_block for device
///////////////////////////////////////////////////////////
static int
ufsd_blkdev_get_block(
    IN struct inode *inode,
    IN sector_t iblock,
    IN OUT struct buffer_head *bh,
    IN int create
    )
{
  bh->b_bdev    = I_BDEV(inode);
  bh->b_blocknr = iblock;
  set_buffer_mapped( bh );
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_bd_read_ahead
//
// Idea from mm/readahead.c
///////////////////////////////////////////////////////////
unsigned long
UFSDAPI_CALL
ufsd_bd_read_ahead(
    IN struct super_block *sb,
    IN unsigned long long offset,
    IN unsigned long      bytes
    )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  struct address_space *mapping = sb->s_bdev->bd_inode->i_mapping;
  usuper    *sbi  = UFSD_SB( sb );
  pgoff_t start   = offset >> PAGE_SHIFT;
  pgoff_t end     = (offset + bytes) >> PAGE_SHIFT;
  pgoff_t end_dev = sbi->dev_size >> PAGE_SHIFT;
  struct list_head page_pool;
  unsigned long nr_pages, nr_anon, nr_free, max_ra;
  struct blk_plug plug;

  nr_anon = global_page_state( NR_ACTIVE_ANON );
  nr_free = global_page_state( NR_FREE_PAGES );
  max_ra  = ( nr_anon + nr_free ) >> 1;

  if ( 0 != sbi->options.raKb ) {
    unsigned long ra = sbi->options.raKb >> ( PAGE_SHIFT-10 );
    if ( max_ra > ra )
      max_ra = ra;
  }

  DebugTrace( 0, UFSD_LEVEL_IO, ("bd_read_ahead: \"%s\", [%llx, + %llx), max_ra=%lx, nr_pages=%lx, anon=%lx, free=%lx)",
              sb->s_id, (UINT64)start, (UINT64)(end - start), max_ra, mapping->nrpages, nr_anon, nr_free ));
//  printk( KERN_WARNING QUOTED_UFSD_DEVICE" bd_read_ahead: \"%s\", [%lx, %lx, %lx)\n", sb->s_id, start, nr_to_read, max_ra );

  if ( 0 == max_ra || start >= end_dev )
    return 0;

  // Check range
  if ( end > end_dev )
    end = end_dev;

  if ( end > start + max_ra )
    end = start + max_ra;

//  printk( KERN_WARNING QUOTED_UFSD_DEVICE" bd_read_ahead: \"%s\", [%lx, %lx)\n", sb->s_id, start, end );

  //
  // Preallocate as many pages as we will need.
  //
  INIT_LIST_HEAD( &page_pool );
  for ( nr_pages = 0; start < end; start++ ) {
    struct page *page;

    spin_lock_irq( &mapping->tree_lock );
    page = radix_tree_lookup( &mapping->page_tree, start );
    spin_unlock_irq( &mapping->tree_lock );

#if is_decl( RADIX_TREE_EXCEPTIONAL_ENTRY )
    if ( NULL != page && !radix_tree_exceptional_entry( page ) )
      continue;
#else
    if ( NULL != page )
      continue;
#endif

#if is_decl( PAGE_CACHE_ALLOC_READAHEAD )
    page = page_cache_alloc_readahead( mapping );
#else
    page = page_cache_alloc_cold( mapping );
#endif
    if ( NULL == page )
      break;

    page->index = start;
    list_add( &page->lru, &page_pool );
    if ( 0 == nr_pages )
      SetPageReadahead( page );
    nr_pages += 1;
  }

  if ( 0 == nr_pages )
    return 0;

  //
  // Now start the IO.  We ignore I/O errors - if the page is not
  // uptodate then the caller will launch readpage again, and
  // will then handle the error.
  //
  blk_start_plug( &plug );

  if ( mapping->a_ops->readpages ) {
    mapping->a_ops->readpages( NULL, mapping, &page_pool, nr_pages );
  } else {
    mpage_readpages( mapping, &page_pool, nr_pages, ufsd_blkdev_get_block );
  }
  put_pages_list( &page_pool );  // Clean up the remaining pages

  blk_finish_plug( &plug );

  DebugTrace( 0, UFSD_LEVEL_IO, ("bd_read_ahead -> %lx", nr_pages ));
//  printk( KERN_WARNING QUOTED_UFSD_DEVICE" bd_read_ahead -> %lx\n", nr_pages );
  return nr_pages << PAGE_SHIFT;
}
#endif // #ifdef UFSD_TURN_OFF_READAHEAD


///////////////////////////////////////////////////////////
// ufsd_bd_unmap_meta
//
// We call this in LookForFreeSpace functions because we can't
// know for sure whether block was used previously or not -
// so we wait for I/O on this block
// @offset: offset in bytes from the beginning of block device
// @bytes: length in bytes
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unmap_meta(
    IN struct super_block *sb,
    IN unsigned long long offset,
    IN unsigned long long bytes
    )
{
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  DEBUG_ONLY( usuper *sbi = UFSD_SB( sb ); )
  struct block_device *bdev = sb->s_bdev;
  sector_t  devblock        = offset >> sb->s_blocksize_bits;
  unsigned long nBlocks     = bytes >> sb->s_blocksize_bits;
  unsigned long cnt         = 0;
  unsigned long limit       = global_page_state( NR_FREE_PAGES ) << (PAGE_SHIFT - sb->s_blocksize_bits);

  if ( limit >= 0x2000 )
    limit -= 0x1000;
  else if ( limit < 32 )
    limit = 32;
  else
    limit >>= 1;

  DebugTrace( 0, UFSD_LEVEL_IO, ("unmap_meta: \"%s\", [%"PSCT"x + %lx)", sb->s_id, devblock, nBlocks ));

  ProfileEnter( sbi, bdunmap_meta );

  while( 0 != nBlocks-- ) {
    clean_bdev_aliases( bdev, devblock++, 1 );
    if ( cnt++ >= limit ) {
      DEBUG_ONLY( sbi->bdunmap_meta_sync += 1; )
      sync_blockdev( bdev );
      cnt = 0;
    }
  }

  ProfileLeave( sbi, bdunmap_meta );

  CheckTime( 1 );
  ufsd_check_sp();
}


///////////////////////////////////////////////////////////
// ufsd_bd_read
//
// Read data from block device
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_read(
    IN  struct super_block *sb,
    IN  UINT64  offset,
    IN  size_t  bytes,
    OUT void    *buffer
    )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
#if defined UFSD_HFS || defined UFSD_DEBUG
  usuper    *sbi        = UFSD_SB( sb );
#endif
  sector_t  devblock    = offset >> sb->s_blocksize_bits;
  size_t    bytes2skip  = ((size_t)offset) & (sb->s_blocksize - 1); // offset % sb->s_blocksize
  unsigned blocksize    = sb->s_blocksize;
  struct block_device *bdev = sb->s_bdev;
  int err               = 0;

#ifdef UFSD_DEBUG
  if ( unlikely( sbi->eject ) ) {
    ufsd_printk( sb, "bd_read ejected block 0x%"PSCT"x", devblock );
    return -EIO;
  }
#endif

  DebugTrace( +1, UFSD_LEVEL_IO, ("bdread: \"%s\", %"PSCT"x, %zx", sb->s_id, devblock, bytes));

  ProfileEnter( sbi, bdread );

  if ( unlikely( !is_bdi_ok( sb ) ) ) {
    err = -ENODEV;
    goto out;
  }

  while ( 0 != bytes ) {
    size_t ToRead;
    struct buffer_head *bh;

#ifdef UFSD_HFS
    if ( devblock == sbi->max_block ) {
      assert( 512 == bytes );
      bh = bh_tail( sb, bytes2skip );
      bytes2skip = 0;
    } else
#endif
    {
      DEBUG_ONLY( if ( 0 != bytes2skip || bytes < sb->s_blocksize ) sbi->nReadBlocksNa += 1; )

      bh = __getblk( bdev, devblock, blocksize );

      if ( NULL != bh && !buffer_uptodate( bh ) ) {
        Ll_rw_block( READ, REQ_META | REQ_PRIO, 1, &bh );
        wait_on_buffer( bh );
        if ( !buffer_uptodate( bh ) ) {
          put_bh( bh );
          bh = NULL;
        }
      }
    }

    if ( NULL == bh ) {
      ufsd_printk( sb, "failed to read block 0x%"PSCT"x", devblock );
      err = -EIO;
      goto out;
    }

    DEBUG_ONLY( sbi->nReadBlocks += 1; )

    ToRead = sb->s_blocksize - bytes2skip;
    if ( ToRead > bytes )
      ToRead = bytes;

#ifdef CopyPage
    if ( likely( PAGE_SIZE == ToRead ) && 0 == ((size_t)buffer & 0x3f) ) {
      assert( 0 == bytes2skip );
      CopyPage( buffer, bh->b_data );
    } else
#endif
      memcpy( buffer, bh->b_data + bytes2skip, ToRead );

    __brelse( bh );

    buffer      = Add2Ptr( buffer, ToRead );
    devblock   += 1;
    bytes      -= ToRead;
    bytes2skip  = 0;
  }

out:
  ProfileLeave( sbi, bdread );

#ifdef UFSD_DEBUG
  if ( ufsd_trace_level & UFSD_LEVEL_IO )
    ufsd_trace_inc( -1 );
#endif
//  DebugTrace( -1, UFSD_LEVEL_IO, ("bdread -> ok"));
  CheckTime( 1 );
  ufsd_check_sp();
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_write
//
// Write data to block device
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_write(
    IN struct super_block *sb,
    IN UINT64       offset,
    IN size_t       bytes,
    IN const void   *buffer,
    IN size_t       wait
    )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  CHECK_TIME_ONLY( size_t bytes0 = bytes; )

#if defined UFSD_HFS || defined UFSD_DEBUG
  usuper    *sbi        = UFSD_SB( sb );
#endif
  sector_t  devblock    = offset >> sb->s_blocksize_bits;
  size_t    bytes2skip  = ((size_t)offset) & (sb->s_blocksize - 1); // offset % sb->s_blocksize
  int  err              = 0;

  if ( !wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    wait = UFSD_RW_WAIT_SYNC;

  if ( is_refs3( &sbi->options ) || is_refs( &sbi->options ) )
  {
    assert( 0 );
    dump_stack();
  }

#ifdef UFSD_DEBUG
  if ( unlikely( sbi->eject ) ) {
    ufsd_printk( sb, "bd_write ejected block 0x%"PSCT"x", devblock );
    return -EIO;
  }
#endif

  DebugTrace( +1, UFSD_LEVEL_IO, ("bdwrite: \"%s\", %"PSCT"x, %zx, %s", sb->s_id, devblock, bytes, wait?", wait":""));

  ProfileEnter( sbi, bdwrite );

  if ( unlikely( !is_bdi_ok( sb ) ) ) {
    err = -ENODEV;
    goto out;
  }

  while ( 0 != bytes ) {

    size_t towrite;
    struct buffer_head *bh;

#ifdef UFSD_HFS
    if ( devblock == sbi->max_block ) {
      assert( bytes == 512 );
      bh = bh_tail( sb, bytes2skip );
      bytes2skip = 0;
    } else
#endif
    {
      DEBUG_ONLY( if ( 0 != bytes2skip || bytes < sb->s_blocksize ) sbi->nWrittenBlocksNa += 1; )

      bh = ( 0 != bytes2skip || bytes < sb->s_blocksize ? __bread : __getblk )( sb->s_bdev, devblock, sb->s_blocksize );
    }

    if ( NULL == bh ) {
      ufsd_printk( sb, "failed to write block 0x%"PSCT"x", devblock );
      err = -EIO;
      goto out;
    }

    if ( buffer_locked( bh ) )
      __wait_on_buffer( bh );

    towrite = sb->s_blocksize - bytes2skip;
    if ( towrite > bytes )
      towrite = bytes;

    //
    // Update buffer with user data
    //
    lock_buffer( bh );
#ifdef CopyPage
    if ( likely( PAGE_SIZE == towrite ) && 0 == ((size_t)buffer & 0x3f) ) {
      assert( 0 == bytes2skip );
      CopyPage( bh->b_data, (void*)buffer ); // copy_page requires source page as non const!
    }  else
#endif
      memcpy( bh->b_data + bytes2skip, buffer, towrite );
    buffer  = Add2Ptr( buffer, towrite );

    set_buffer_uptodate( bh );
    mark_buffer_dirty( bh );
    unlock_buffer( bh );

    DEBUG_ONLY( sbi->nWrittenBlocks += 1; )

    if ( wait ) {
#ifdef UFSD_DEBUG
      if ( !(ufsd_trace_level & UFSD_LEVEL_IO) )
        DebugTrace( 0, UFSD_LEVEL_VFS, ("bdwrite(wait), bh=%"PSCT"x", devblock));
#endif

      err = sync_dirty_buffer( bh );

      if ( 0 != err ) {
        ufsd_printk( sb, "failed to sync buffer at block %" PSCT "x, error %d", bh->b_blocknr, err );
        __brelse( bh );
        goto out;
      }
    }

    __brelse( bh );

    devblock    += 1;
    bytes       -= towrite;
    bytes2skip   = 0;
  }

out:
  ProfileLeave( sbi, bdwrite );

#ifdef UFSD_DEBUG
  if ( ufsd_trace_level & UFSD_LEVEL_IO )
    ufsd_trace_inc( -1 );
#endif
//  DebugTrace( -1, UFSD_LEVEL_IO, ("bd_write -> ok"));
  CheckTimeEx( 1, "%llx,%zx", offset, bytes0 );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_map
//
//
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_map(
    IN  struct super_block *sb,
    IN  UINT64  offset,
    IN  size_t  bytes,
    IN  size_t  flags,
    OUT struct buffer_head **bcb,
    OUT void    **mem
    )
{
  struct buffer_head *bh;
#if defined UFSD_DEBUG || defined UFSD_HFS
  usuper *sbi = UFSD_SB( sb );
#endif
  unsigned blocksize  = sb->s_blocksize;
  sector_t  devblock  = (sector_t)(offset >> sb->s_blocksize_bits);
  size_t bytes2skip   = (size_t)(offset & (blocksize - 1)); // offset % sb->s_blocksize
  DEBUG_ONLY( const char *hint; )
  DEBUG_ONLY( const char *hint2 = ""; )
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )

  if ( bytes2skip + bytes > blocksize ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdmap: [%llx %zx] overlaps block boundary %x", offset, bytes, blocksize));
    return -EINVAL;
  }

#ifdef UFSD_DEBUG
  if ( unlikely( sbi->eject ) ) {
    ufsd_printk( sb, "bd_map ejected block 0x%"PSCT"x", devblock );
    return -EIO;
  }
#endif

  ProfileEnter( sbi, bdmap );

#ifdef UFSD_HFS
  if ( devblock == sbi->max_block ) {
    assert( bytes == 512 );
    bh = bh_tail( sb, bytes2skip );
    bytes2skip = 0;
    DEBUG_ONLY( hint = "tail "; )
  } else
#endif
  {
    bh = __getblk( sb->s_bdev, devblock, blocksize );
    if ( NULL == bh ) {
      DEBUG_ONLY( hint = ""; ) // to suppress some compiler warnings
    } else if ( 0 == bytes2skip && bytes == blocksize && FlagOn( flags, UFSD_RW_MAP_NO_READ ) ) {
      DEBUG_ONLY( hint = "n "; )
      set_buffer_uptodate( bh );
    } else if ( buffer_uptodate( bh ) ) {
      DEBUG_ONLY( hint = "c "; )
    } else {
      DEBUG_ONLY( hint = "r "; )
      Ll_rw_block( READ, REQ_META | REQ_PRIO, 1, &bh );
      wait_on_buffer( bh );
      if ( !buffer_uptodate( bh ) ) {
        put_bh( bh );
        bh = NULL;
      }
    }
  }

  ProfileLeave( sbi, bdmap );

  if ( NULL == bh ) {
    ufsd_printk( sb, "failed to map block 0x%"PSCT"x", devblock );
    return -EIO;
  }

  if ( buffer_locked( bh ) ) {
    DEBUG_ONLY( hint2 = " w"; )
    __wait_on_buffer( bh );
  }

  DebugTrace( 0, UFSD_LEVEL_IO, ("bdmap: \"%s\", %"PSCT"x, %zx, %s%s%s -> %p (%d)", sb->s_id, devblock, bytes,
              hint, buffer_dirty( bh )?"d":"c", hint2, bh, atomic_read( &bh->b_count ) ));

  //
  // Return pointer into page
  //
  *mem = Add2Ptr( bh->b_data, bytes2skip );
  *bcb = bh;

#ifdef UFSD_DEBUG
  sbi->nMappedBh += 1;
  assert( sbi->nMappedBh >= sbi->nUnMappedBh );
  {
    size_t buffers = sbi->nMappedBh - sbi->nUnMappedBh;
    if ( buffers > sbi->nPeakMappedBh )
      sbi->nPeakMappedBh = buffers;
  }
#endif

  CheckTime( 1 );
  ufsd_check_sp();
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_bd_unmap
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unmap(
#ifdef UFSD_DEBUG
    IN struct super_block *sb,
#endif
    IN struct buffer_head *bh,
    IN int Forget
    )
{
  DebugTrace( 0, UFSD_LEVEL_IO, ("bdunmap: \"%s\", %"PSCT"x,%s %d", sb->s_id, bh->b_blocknr, buffer_dirty( bh )?"d":"c", atomic_read( &bh->b_count ) - 1 ));
  (Forget?__bforget : __brelse)( bh );

  DEBUG_ONLY( UFSD_SB( sb )->nUnMappedBh += 1; )
}


///////////////////////////////////////////////////////////
// ufsd_bd_set_dirty
//
// Mark buffer as dirty and sync it if necessary
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_set_dirty(
    IN struct super_block *sb,
    IN struct buffer_head *bh,
    IN size_t   wait
    )
{
  int err = 0;
#if defined UFSD_HFS || defined UFSD_DEBUG
  usuper *sbi = UFSD_SB( sb );
#endif

  if ( unlikely( !is_bdi_ok( sb ) ) )
    return -ENODEV;

  if ( !wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    wait = UFSD_RW_WAIT_SYNC;

  DebugTrace( 0, UFSD_LEVEL_IO, ("bddirty: \"%s\", %"PSCT"x,%s %d", sb->s_id, bh->b_blocknr, buffer_dirty( bh )?"d":"c", atomic_read( &bh->b_count ) ));
  set_buffer_uptodate( bh );
  mark_buffer_dirty( bh );

  if ( wait ) {
    ProfileEnter( sbi, bdsetdirty );
    err = sync_dirty_buffer( bh );
    if ( 0 != err )
      ufsd_printk( sb, "failed to sync buffer at block %" PSCT "x, error %d", bh->b_blocknr, err );
    ProfileLeave( sbi, bdsetdirty );
  }
#ifdef UFSD_HFS
  else if ( bh->b_blocknr >= sbi->max_block ) {
    DebugTrace( 0, UFSD_LEVEL_IO, ("write tail: %"PSCT"x", bh->b_blocknr ));
    lock_buffer( bh );
    Submit_bh( WRITE, 0, bh );
  }
#endif

  return err;
}

#if (defined UFSD_REFS || defined UFSD_REFS3) && defined ufsd_buf_get

#define UFSD_BUF_FLAG_READ    (1 << 0)
#define UFSD_BUF_FLAG_WRITE   (1 << 1)
#define UFSD_BUF_FLAG_PAGES   (1 << 20) // alloc_page
#define UFSD_BUF_FLAG_KMEM    (1 << 21) // kmalloc


typedef struct ufsd_buf {

  unsigned long long  lbo;          // Lbo of buffer
  unsigned int        bytes;        // size of buffer
  atomic_t            count;        // reference count
  void                *addr;        // virtual address of buffer
  unsigned long       flags;        // UFSD_BUF_FLAG_XXX

  spinlock_t          lock;         // internal state lock
  int                 error;        //
  wait_queue_head_t   waiters;      // unpin waiters

  struct completion   iowait;       // queue for I/O waiters
  struct page         **pages;      // array of page pointers
  struct page         *page_array[4];  // inline pages
  atomic_t            io_remaining; //  outstanding I/O requests
  unsigned int        page_count;   //  size of page array
  unsigned int        offset;       //  page offset in first page

} ufsd_buf;

// Return true if the buffer is vmapped
#define ufsd_buf_is_vmapped( ub )   (ub)->page_count > 1
// Return the length of mapped area
#define ufsd_buf_vmap_len( ub ) ((ub)->bytes) - (ub)->offset


///////////////////////////////////////////////////////////
// ufsd_buf_alloc
//
//
///////////////////////////////////////////////////////////
static ufsd_buf*
ufsd_buf_alloc(
    IN unsigned long long lbo,
    IN unsigned int       bytes
    )
{
  unsigned long i, page_count;
  ufsd_buf  *ub = kmalloc( sizeof(ufsd_buf), __GFP_ZERO|GFP_NOFS );
  if ( unlikely( NULL == ub ) )
    return NULL;

  ub->lbo   = lbo;
  ub->bytes = bytes;
  atomic_set( &ub->count, 1 );
  spin_lock_init( &ub->lock );
  init_waitqueue_head( &ub->waiters );
  init_completion( &ub->iowait );

  if ( bytes < PAGE_SIZE ) {
    size_t addr = (size_t)kmalloc( bytes, __GFP_ZERO|GFP_NOFS );
    if ( likely( 0 != addr ) ) {
      if ( ((addr + bytes - 1) & PAGE_MASK) == (addr & PAGE_MASK) ) {
        ub->addr        = (void*)addr;
        ub->offset      = offset_in_page( ub->addr );
        ub->pages       = ub->page_array;
        ub->pages[0]    = virt_to_page( ub->addr );
        ub->page_count  = 1;
        SetFlag( ub->flags, UFSD_BUF_FLAG_KMEM );
        goto out;
      }

      // addr spans two pages
      kfree( (void*)addr );
    }
  }

  page_count = ((lbo + bytes + PAGE_SIZE - 1) >> PAGE_SHIFT) - (lbo >> PAGE_SHIFT);
  if ( page_count <= ARRSIZE(ub->page_array) ) {
    ub->pages = ub->page_array;
  } else {
    ub->pages = kmalloc( sizeof(struct page*) * page_count, __GFP_ZERO|GFP_NOFS );
    if ( unlikely( NULL == ub->pages ) ) {
      kfree( ub );
      return NULL;
    }
  }

  ub->page_count = page_count;
  SetFlag( ub->flags, UFSD_BUF_FLAG_PAGES );

  for ( i = 0; i < page_count; i++ ) {
    unsigned retries;

    for ( retries = 0; ; retries++ ) {
      struct page *page = alloc_page( __GFP_NOWARN );
      if ( likely( NULL != page ) ) {
        ub->pages[i] = page;
        break;
      }

      if ( 0 == (retries % 100) )
        ufsd_printk( NULL, "possible memory allocation deadlock\n" );
      congestion_wait( BLK_RW_ASYNC, HZ/50 );
    }
  }

  if ( 1 == page_count ) {
    // A single page buffer is always mappable
    ub->addr = page_address( ub->pages[0] );
  } else {
#ifdef PF_MEMALLOC_NOIO
    unsigned noio_flag = memalloc_noio_save();
    for ( i = 0; i < 2; i++ ) {
      /*
       * vm_map_ram() will allocate auxillary structures (e.g.
       * pagetables) with GFP_KERNEL, yet we are likely to be under
       * GFP_NOFS context here. Hence we need to tell memory reclaim
       * that we are in such a context via PF_MEMALLOC_NOIO to prevent
       * memory reclaim re-entering the filesystem here and
       * potentially deadlocking.
       */
      ub->addr = vm_map_ram( ub->pages, ub->page_count, -1, PAGE_KERNEL );
      if ( NULL != ub->addr )
        break;
//      vm_unmap_aliases();
    }
    memalloc_noio_restore( noio_flag );
#else
    ub->addr = vm_map_ram( ub->pages, ub->page_count, -1, PAGE_KERNEL );
#endif

    if ( NULL == ub->addr ) {
      //
      // Undo all actions in this function
      //
      for ( i = 0; i < page_count; i++ )
        __free_page( ub->pages[i] );
      if ( ub->pages != ub->page_array )
        kfree( ub->pages );
      kfree( ub );
      return NULL;
    }
  }

  ub->offset = (unsigned long)ub->addr & (PAGE_SIZE-1);
  ub->addr  += ub->offset;

out:
//  DebugTrace( 0, 0, ("+buf: %llx + %x: %p, %x, %x\n", ub->lbo, ub->bytes, ub->addr, ub->offset, ub->page_count ));

  return ub;
}


///////////////////////////////////////////////////////////
// ufsd_buf_free
//
//
///////////////////////////////////////////////////////////
static void
ufsd_buf_free(
    IN ufsd_buf *ub
    )
{
//  DebugTrace( 0, 0, ("-buf: %llx + %x: %p, %x, %x\n", ub->lbo, ub->bytes, ub->addr, ub->offset, ub->page_count ));
  if ( FlagOn( ub->flags, UFSD_BUF_FLAG_PAGES ) ) {
    unsigned i;

    if ( ufsd_buf_is_vmapped( ub ) )
      vm_unmap_ram( ub->addr - ub->offset, ub->page_count );

    for ( i = 0; i < ub->page_count; i++ )
      __free_page( ub->pages[i] );
  } else if ( FlagOn( ub->flags, UFSD_BUF_FLAG_KMEM ) )
    kfree( ub->addr );

  if ( ub->pages != ub->page_array )
    kfree( ub->pages );

  kfree( ub );
}


///////////////////////////////////////////////////////////
// ufsd_buf_bio_end_io
//
//
///////////////////////////////////////////////////////////
static void
ufsd_buf_bio_end_io(
    IN struct bio *bio
#ifdef BIO_UPTODATE
    , IN int        error
#endif
    )
{
  ufsd_buf  *ub = bio->bi_private;
  int err       = BIO_RESULT( bio );

  if ( 0 == ub->error )
    ub->error = err;

  if ( !ub->error && ufsd_buf_is_vmapped( ub ) && FlagOn( ub->flags, UFSD_BUF_FLAG_READ ) ) {
#if defined HAVE_DECL_INVALIDATE_KERNEL_VMAP_RANGE_V2 && HAVE_DECL_INVALIDATE_KERNEL_VMAP_RANGE_V2
    invalidate_kernel_vmap_range( (size_t)ub->addr, ufsd_buf_vmap_len(ub) );
#else
    invalidate_kernel_vmap_range( ub->addr, ufsd_buf_vmap_len(ub) );
#endif
  }

  if ( 1 == atomic_dec_and_test( &ub->io_remaining ) )
    complete( &ub->iowait );

  bio_put( bio );
}


///////////////////////////////////////////////////////////
// ufsd_buf_submit
//
// Synchronous buffer IO submission path, read or write.
///////////////////////////////////////////////////////////
static int
ufsd_buf_submit(
    IN struct block_device *bdev,
    IN ufsd_buf *ub
    )
{
  struct blk_plug plug;
  int err;
  int rw                    = FlagOn( ub->flags, UFSD_BUF_FLAG_WRITE )? WRITE : READ;
  unsigned long page_index  = 0;
  unsigned long nr_pages    = ub->page_count;
  unsigned int  offset      = ub->offset;
  unsigned long bytes       = ub->bytes;
  sector_t sector           = ub->lbo >> 9;

  // clear error state
  ub->error = 0;

  atomic_inc( &ub->count );
  atomic_set( &ub->io_remaining, 1 );

  blk_start_plug(&plug);

  while ( 0 != bytes ) {
    //
    // Create new bio
    //
    struct bio  *bio = bio_alloc( GFP_NOIO, min_t( unsigned, bio_get_nr_vecs( bdev ), min_t( unsigned, BIO_MAX_PAGES, nr_pages ) ) );
    if ( NULL == bio ) {
      ub->error = -ENOMEM;
      break;
    }

    atomic_inc( &ub->io_remaining );

    BIO_BISECTOR( bio ) = sector;
    bio->bi_bdev        = bdev;
    bio->bi_end_io      = ufsd_buf_bio_end_io;
    bio->bi_private     = ub;

    // Add pages
    do {
      unsigned to_add = PAGE_SIZE - offset;
      if ( to_add > bytes )
        to_add = bytes;

      if ( bio_add_page( bio, ub->pages[page_index], to_add, offset ) < to_add )
        break;

      offset       = 0;
      bytes       -= to_add;
      sector      += to_add >> 9;
      page_index  += 1;
      nr_pages    -= 1;
    } while( 0 != bytes );

    if ( ufsd_buf_is_vmapped( ub ) ) {
#if defined HAVE_DECL_INVALIDATE_KERNEL_VMAP_RANGE_V2 && HAVE_DECL_INVALIDATE_KERNEL_VMAP_RANGE_V2
      flush_kernel_vmap_range( (size_t)ub->addr, ufsd_buf_vmap_len( ub ) );
#else
      flush_kernel_vmap_range( ub->addr, ufsd_buf_vmap_len( ub ) );
#endif
    }

    Submit_bio( rw, REQ_META, bio );
  }

  blk_finish_plug( &plug );

  // make sure we run completion synchronously if it raced with us and is already complete.
  if ( 1 == atomic_dec_and_test( &ub->io_remaining ) )
    complete( &ub->iowait );

  // wait for completion before gathering the error from the buffer
  wait_for_completion( &ub->iowait );
  err = ub->error;

  // all done now, we can release the hold that keeps the buffer referenced for the entire IO.
  if ( atomic_dec_and_test( &ub->count ) )
    ufsd_buf_free( ub );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_buf_get
//
//
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_buf_get(
    IN  struct super_block *sb,
    IN  UINT64    offset,
    IN  unsigned  bytes,
    IN  unsigned  flags,
    OUT void      **bcb,
    OUT void      **mem
    )
{
  int err;
  ufsd_buf* ub;
#if defined UFSD_DEBUG
  usuper *sbi = UFSD_SB( sb );
#endif

  DebugTrace( 0, UFSD_LEVEL_IO, ("%s: \"%s\", %llx, %x\n", FlagOn( flags, UFSD_RW_MAP_NO_READ )? "buf_get" : "buf_read", sb->s_id, offset, bytes ));

  ProfileEnter( sbi, buf_get );

  ub = ufsd_buf_alloc( offset, bytes );
  if ( NULL == ub )
    err = -ENOMEM;
  else {
    if ( FlagOn( flags, UFSD_RW_MAP_NO_READ ) ) {
      memset( Add2Ptr( ub->addr, ub->offset ), 0, bytes );
      err = 0;
    } else {
      SetFlag( ub->flags, UFSD_BUF_FLAG_READ );
      err = ufsd_buf_submit( sb->s_bdev, ub );
      if ( 0 != err )
        ufsd_buf_free( ub );
    }
  }

  ProfileLeave( sbi, buf_get );

  if ( 0 == err ) {
    *mem = ub->addr;
    *bcb = ub;
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_buf_put
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_buf_put(
    IN struct super_block *sb,
    IN void* bcb
    )
{
  ufsd_buf *ub = bcb;
#if defined UFSD_DEBUG
  usuper *sbi = UFSD_SB( sb );
#endif

  DebugTrace( 0, UFSD_LEVEL_IO, ("buf_put: \"%s\", %llx, %d\n", sb->s_id, ub->lbo, atomic_read( &ub->count ) - 1 ));

  ProfileEnter( sbi, buf_put );

  if ( atomic_dec_and_test( &ub->count ) )
    ufsd_buf_free( ub );

  ProfileLeave( sbi, buf_put );
}


///////////////////////////////////////////////////////////
// ufsd_buf_write
//
//
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_buf_write(
    IN struct super_block *sb,
    IN void   *bcb,
    IN int    wait
    )
{
  int err;
  ufsd_buf *ub = bcb;
#if defined UFSD_DEBUG
  usuper *sbi = UFSD_SB( sb );
#endif

  if ( unlikely( !is_bdi_ok( sb ) ) )
    return -ENODEV;

  if ( !wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    wait = UFSD_RW_WAIT_SYNC;

  DebugTrace( 0, UFSD_LEVEL_IO, ("buf_write: \"%s\", %llxx, %d\n", sb->s_id, ub->lbo, atomic_read( &ub->count ) ));
  SetFlag( ub->flags, UFSD_BUF_FLAG_WRITE );

  ProfileEnter( sbi, buf_write );

  err = ufsd_buf_submit( sb->s_bdev, ub );

  ProfileLeave( sbi, buf_write );

  return err;
}
#endif // #if (defined UFSD_REFS || defined UFSD_REFS3 ) && defined ufsd_buf_get


#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_bd_lock_buffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_lock_buffer(
    IN struct buffer_head *bh
    )
{
  assert( NULL != bh );
  assert( !buffer_locked( bh ) );
  lock_buffer( bh );
}


///////////////////////////////////////////////////////////
// ufsd_bd_unlock_buffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unlock_buffer(
    IN struct buffer_head *bh
    )
{
  assert( NULL != bh );
  assert( buffer_locked( bh ) );
  set_buffer_uptodate( bh );
  unlock_buffer( bh );
}
#endif


///////////////////////////////////////////////////////////
// ufsd_bd_discard
//
// Issue a discard request (trim for SSD)
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_discard(
    IN struct super_block *sb,
    IN UINT64 offset,
    IN UINT64 bytes
    )
{
  usuper *sbi = UFSD_SB( sb );
  if ( FlagOn( sbi->flags, UFSD_SBI_FLAGS_DISRCARD ) && sbi->options.discard ) {
    int err;
    // Align up 'start' on discard_granularity
    UINT64 start  = (offset + sbi->discard_granularity - 1) & sbi->discard_granularity_mask_inv;
    // Align down 'end' on discard_granularity
    UINT64 end    = (offset + bytes) & sbi->discard_granularity_mask_inv;
    UINT64 len;
    CHECK_TIME_ONLY( unsigned long j0 = jiffies; )

    if ( start >= end ) {
      DebugTrace(0, UFSD_LEVEL_IO, ("discard: \"%s\", %llx, %llx => nothing due to granularity", sb->s_id, offset, bytes ));
      return 0;
    }

    len = end - start;

    ProfileEnter( sbi, bd_discard );

    err = blkdev_issue_discard( sb->s_bdev, start >> 9, len >> 9, GFP_NOFS, 0 );

    ProfileLeave( sbi, bd_discard );

    if ( -EOPNOTSUPP == err ) {
      DebugTrace(-1, UFSD_LEVEL_IO, ("discard -> not supported"));
      ClearFlag( sbi->flags, UFSD_SBI_FLAGS_DISRCARD );
      return ERR_NOTIMPLEMENTED;
    }

    DebugTrace(0, UFSD_LEVEL_IO, ("discard: \"%s\", %llx, %llx (%llx, %llx) -> %d", sb->s_id, offset, bytes, start, len, err ));

    CheckTime( 1 );
    ufsd_check_sp();
    return 0 == err? 0 : ERR_BADPARAMS;
  }
  return ERR_NOTIMPLEMENTED;
}

#if is_decl( BLKDEV_ISSUE_ZEROOUT_V1 ) // ~ Linux v2.6.x
  #define Blkdev_issue_zeroout( __SB, __OFFT, __BYTES )    blkdev_issue_zeroout( __SB->s_bdev, __OFFT, __BYTES, GFP_NOFS, BLKDEV_IFL_WAIT | BLKDEV_IFL_BARRIER )
#elif is_decl( BLKDEV_ISSUE_ZEROOUT_V2 ) // ~ Linux v3.x
  #define Blkdev_issue_zeroout( __SB, __OFFT, __BYTES )    blkdev_issue_zeroout( __SB->s_bdev, __OFFT, __BYTES, GFP_NOFS )
#elif is_decl( BLKDEV_ISSUE_ZEROOUT_V3 ) // ~ Linux v4.x
  #define Blkdev_issue_zeroout( __SB, __OFFT, __BYTES )    blkdev_issue_zeroout( __SB->s_bdev, __OFFT, __BYTES, GFP_NOFS, ( UFSD_SB( __SB ) )->options.discard )
#endif

///////////////////////////////////////////////////////////
// ufsd_bd_zero
//
// Helper function to zero blocks in block device
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_zero(
    IN struct super_block *sb,
    IN UINT64 offset,
    IN UINT64 bytes
    )
{
//  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  int err;
  DEBUG_ONLY( usuper *sbi = UFSD_SB( sb ); )

  // must be 512 bytes aligned
  assert( 0 == (offset&0x1ff) );
  assert( 0 == (bytes&0x1ff) );

  ProfileEnter( sbi, bd_zero );

  err = Blkdev_issue_zeroout( sb, offset >> 9, bytes >> 9 );

  ProfileLeave( sbi, bd_zero );

  DebugTrace(0, UFSD_LEVEL_IO, ("bdzero: \"%s\", %llx, %llx -> %d", sb->s_id, offset, bytes, err ));
//  CheckTimeEx( 5, "%llx,%llx", offset, bytes );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_set_blocksize
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_set_blocksize(
    IN struct super_block *sb,
    IN unsigned int BytesPerBlock
    )
{
  usuper *sbi = UFSD_SB( sb );

  if ( BytesPerBlock <= PAGE_SIZE ) {
    sb_set_blocksize( sb, BytesPerBlock );
    sbi->max_block      = sb->s_bdev->bd_inode->i_size >> sb->s_blocksize_bits;
  }

  DebugTrace( 0, Dbg, ("BdSetBlockSize %x -> %lx", BytesPerBlock, sb->s_blocksize ));
}


///////////////////////////////////////////////////////////
// ufsd_bd_isreadonly
//
// Returns !0 for readonly media
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_isreadonly(
    IN struct super_block *sb
    )
{
  return FlagOn( sb->s_flags, MS_RDONLY );
}


///////////////////////////////////////////////////////////
// ufsd_bd_barrier
//
//
///////////////////////////////////////////////////////////
static void
ufsd_bd_barrier(
    IN struct super_block *sb
    )
{
  struct block_device *bdev = sb->s_bdev;
  usuper *sbi = UFSD_SB( sb );

  if ( !sbi->options.nobarrier ) {
    int err = Blkdev_issue_flush( bdev );
    if ( -EOPNOTSUPP == err ) {
      printk( KERN_WARNING QUOTED_UFSD_DEVICE": disabling barriers on \"%s\" - not supported\n", sb->s_id );
      sbi->options.nobarrier = 1;
    }
  }
}


///////////////////////////////////////////////////////////
// ufsd_bd_flush
//
//
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_flush(
    IN struct super_block *sb,
    IN unsigned wait
    )
{
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  int err;
  struct block_device *bdev = sb->s_bdev;
  DEBUG_ONLY( usuper *sbi = UFSD_SB( sb ); )

  DebugTrace( 0, Dbg, ("bdflush \"%s\"", sb->s_id ));

  ProfileEnter( sbi, bdflush );

  err = sync_blockdev( bdev );

  if ( 0 != err ) {
    ufsd_printk( sb, "bdflush -> %d", err );
  } else if ( wait ) {
    ufsd_bd_barrier( sb );
  }

  ProfileLeave( sbi, bdflush );

  CheckTime( 1 );
  ufsd_check_sp();
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_get_discard
//
// Returns the size of discard block
///////////////////////////////////////////////////////////
unsigned int
UFSDAPI_CALL
ufsd_bd_get_discard(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );
  unsigned int ret = sbi->discard_granularity;
  DebugTrace( 0, Dbg, ("get_discard(\"%s\") -> %x", sb->s_id, ret ));
  return ret;
}


//
// Delayed write
//
typedef struct delay_write_inode{
  struct list_head  wlist;
  finfo       fi;
  ufsd_file   *ufile;
  unsigned    ia_valid;

} delay_write_inode;


#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_on_close_file
//
// update sbi->clear_list && sbi->write_list
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_on_close_file(
    IN struct super_block *sb,
    IN ufsd_file          *ufile
    )
{
  struct list_head* pos;
  usuper *sbi = UFSD_SB( sb );
  delay_write_inode* to_free = NULL;
  int ret = 0;

  spin_lock( &sbi->ddt_lock );
  list_for_each( pos, &sbi->write_list ) {
    delay_write_inode *dw = list_entry( pos, delay_write_inode, wlist );
    if ( dw->ufile == ufile ) {
      list_del(  pos );
      to_free = dw;
      ret = 1;
      break;
    }
  }

  list_for_each( pos, &sbi->clear_list ) {
    ufsd_file *file = (ufsd_file*)( (char*)pos - usdapi_file_to_list_offset() );
    if ( file == ufile ) {
      list_del( pos );
#ifndef NDEBUG
      INIT_LIST_HEAD( pos );
#endif
      ret |= 2;
      break;
    }
  }
  spin_unlock( &sbi->ddt_lock );

  if ( NULL != to_free )
    kfree( to_free );
  assert( 0 == (ret & 2) ); // to debug the case
  return ret;
}
#endif


///////////////////////////////////////////////////////////
// do_delayed_tasks
//
// This function is called under locked api_mutex
///////////////////////////////////////////////////////////
static void
do_delayed_tasks(
    IN usuper *sbi
    )
{
  unsigned int cnt;
  int VFlush = atomic_read( &sbi->VFlush );
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )

  if ( 0 != VFlush || ( sbi->options.sync && ufsdapi_is_volume_dirty( sbi->ufsd ) ) ) {
    ufsdapi_volume_flush( sbi->ufsd, 2 == VFlush );
    atomic_set( &sbi->VFlush, 0 );

    CheckTimeEx( 2, "flush vol" );
    CHECK_TIME_ONLY( j0 = jiffies; )
  }

  //
  // Do delayed write
  //
  for ( cnt = 0; ; cnt++ ) {
    delay_write_inode *dw;

    spin_lock( &sbi->ddt_lock );
    if ( list_empty( &sbi->write_list ) ) {
      dw = NULL;
    } else {
      dw = list_entry( sbi->write_list.next, delay_write_inode, wlist );
      list_del( &dw->wlist );
    }
    spin_unlock( &sbi->ddt_lock );

    if ( NULL == dw ) {
      if ( 0 != cnt ) {
        DebugTrace( 0, Dbg, ("do_delayed_tasks: write=%u", cnt ) );

        CheckTimeEx( 2, "flush file" );
        CHECK_TIME_ONLY( j0 = jiffies; )
      }
      break;
    }

    ufsdapi_file_flush( sbi->ufsd, dw->ufile, &dw->fi, dw->ia_valid, NULL, 0, NULL );

    kfree( dw );
  }

  //
  // Do delayed clear
  //
  for ( cnt = 0; ; cnt++ ) {
    ufsd_file *file;
    spin_lock( &sbi->ddt_lock );
    if ( list_empty( &sbi->clear_list ) ) {
      file = NULL;
    } else {
      struct list_head* lh = sbi->clear_list.next;
      file = (ufsd_file*)( (char*)lh - usdapi_file_to_list_offset() );
      list_del( lh );
#ifndef NDEBUG
      INIT_LIST_HEAD( lh );
#endif
    }
    spin_unlock( &sbi->ddt_lock );

    if ( NULL == file ) {
      if ( 0 != cnt ) {
        DebugTrace( 0, Dbg, ("do_delayed_tasks: clear=%u", cnt ) );
        CheckTimeEx( 2, "clear file" );
      }
      break;
    }

    ufsdapi_file_close( sbi->ufsd, file );
  }
}


///////////////////////////////////////////////////////////
// _lock_ufsd
//
//
///////////////////////////////////////////////////////////
static void
_lock_ufsd(
    IN usuper *sbi
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
  DEBUG_ONLY( unsigned long dT; )
  DEBUG_ONLY( unsigned long T0 = jiffies; )

  mutex_lock( &sbi->api_mutex );

  CHECK_TIME_ONLY( sbi->lock_time = jiffies; )

  DEBUG_ONLY( dT         = jiffies - T0; )
  DEBUG_ONLY( WaitMutex += dT; )

  ufsd_check_sp_start( Hint );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sbi->sys_info );
    ufsd_trace( "%u: %lx %lx \"%s\" %s (+)",
                jiffies_to_msecs(jiffies-StartJiffies),
                sbi->sys_info.freeram, sbi->sys_info.bufferram,
                current->comm, Hint );

    ufsd_trace_inc( 1 );
  }
#endif

#ifdef UFSD_CHECK_TIME
  if ( dT > 5*HZ ) {
    DebugTrace( 0, 0, ("**** ufsd wait for lock too long %u: %s", jiffies_to_msecs( dT ), Hint ));
    dump_stack();
  }
#endif

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sbi );
}


///////////////////////////////////////////////////////////
// try_lock_ufsd
//
// Returns 1 if mutex is locked
///////////////////////////////////////////////////////////
static int
_try_lock_ufsd(
    IN usuper *sbi
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
  int ok = mutex_trylock( &sbi->api_mutex );
  assert( 0 == ok || 1 == ok );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sbi->sys_info );
    ufsd_trace( "%u: %lx %lx \"%s\" %s %s",
                jiffies_to_msecs(jiffies-StartJiffies),
                sbi->sys_info.freeram, sbi->sys_info.bufferram,
                current->comm, Hint, ok? "(+)" : "-> wait" );
    if ( ok )
      ufsd_trace_inc( 1 );
  }
#endif

  if ( !ok )
    return 0;

  CHECK_TIME_ONLY( sbi->lock_time = jiffies; )
  ufsd_check_sp_start( Hint );

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sbi );

  return 1;
}


///////////////////////////////////////////////////////////
// unlock_ufsd
//
//
///////////////////////////////////////////////////////////
static void
_unlock_ufsd(
    IN usuper *sb
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sb );

  ufsd_check_sp();

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sb->sys_info );
    ufsd_trace_inc( -1 );
    ufsd_trace( "%u: %lx %lx \"%s\" %s (-)",
                jiffies_to_msecs(jiffies-StartJiffies),
                sb->sys_info.freeram, sb->sys_info.bufferram,
                current->comm, Hint );
  }
#endif

#ifdef UFSD_CHECK_TIME
  if ( jiffies - sb->lock_time > 5*HZ ) {
    DebugTrace( 0, 0, ("**** ufsd locked too long %u: %s, %s", jiffies_to_msecs(jiffies - sb->lock_time), current->comm, Hint ));
    dump_stack();
  }
#endif

  mutex_unlock( &sb->api_mutex );
}


#if ( defined UFSD_NTFS || defined UFSD_HFS ) && defined UFSD_CLOSE_AT_RELEASE
///////////////////////////////////////////////////////////
// ufsd_open_by_id
//
// Assumed lock_ufsd()
// Returns 0 if OK
///////////////////////////////////////////////////////////
static int
ufsd_open_by_id(
    IN usuper       *sbi,
    IN struct inode *i
    )
{
  finfo *fi;
  unode *u = UFSD_U(i);
  if ( NULL != u->ufile )
    return 0;

  assert( is_ntfs( &sbi->options ) || is_hfs( &sbi->options ) );

  if ( 0 == ufsdapi_file_open_by_id( sbi->ufsd, i->i_ino, &u->ufile, &fi ) ) {
    assert( NULL != u->ufile );
    assert( i->i_ino == fi->Id );

    if ( S_ISDIR( i->i_mode ) == FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
      //
      // This is the first request for 'inode' that requires ufsd. ufsd is locked at this moment.
      // Should we use an additional lock?
      //
      unsigned long flags;

      assert( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) || fi->ValidSize <= fi->FileSize );

      write_lock_irqsave( &u->rwlock, flags );
      // Since we read MFT here, and u->flags may contain some flags from
      // dir entry (which could contain other flags set), we need to clear
      // all UFSD_UNODE_FLAG_API_FLAGS in u->flags and then set flags from
      // fi->Attrib.
      u->flags = (u->flags & ~UFSD_UNODE_FLAG_API_FLAGS) | (fi->Attrib & UFSD_UNODE_FLAG_API_FLAGS);

      i_size_write( i, fi->FileSize );
      inode_set_bytes( i, fi->AllocSize );

      if ( !FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
        set_nlink( i, fi->HardLinks );
        u->valid    = fi->ValidSize;
      }
      write_unlock_irqrestore( &u->rwlock, flags );
      return 0;
    }

    // bad inode
    ufsd_printk( i->i_sb, "Incorrect dir/file of inode r=%lx", i->i_ino );
  }

  make_bad_inode( i );
  return -ENOENT;
}
#else
#define ufsd_open_by_id( sbi, i ) 0
#endif


#if defined UFSD_NTFS && defined ufsd_get_page0
///////////////////////////////////////////////////////////
// ufsd_get_page0
//
// Fills the current user's data from page 0
///////////////////////////////////////////////////////////
unsigned  UFSDAPI_CALL
ufsd_get_page0(
    IN void   *inode,
    OUT void  *data,
    IN unsigned int bytes
    )
{
  struct inode  *i  = (struct inode*)inode;
  struct page *page = find_lock_page( i->i_mapping, 0 );
  unsigned ret = 0;
  if ( NULL == page )
    return 0;

  if ( bytes <= PAGE_SIZE && PageUptodate( page )
    && ( !page_has_buffers( page ) || try_to_release_page( page, 0 ) ) ) {

    unsigned long flags;
    unode *u    = UFSD_U( i );
    void* kaddr = atomic_kmap( page );
    memcpy( data, kaddr, bytes );
    atomic_kunmap( kaddr );

    assert( !page_has_buffers( page ) );

    // file becames resident
    write_lock_irqsave( &u->rwlock, flags );
    u->len = 0;
    inode_set_bytes( i, 0 );
    write_unlock_irqrestore( &u->rwlock, flags );

    ret  = bytes;

    if ( 0 != ret ) {
      wait_on_page_writeback( page );
      SetPageUptodate( page );
      ClearPageDirty( page );
    }
  }
  unlock_page( page );
  DebugTrace( 0, Dbg, ("**** ufsd_get_page0 r=%lx, %x, pf=%lx -> %x", i->i_ino, bytes, page->flags, ret ));
  put_page( page );
  return ret;
}
#endif


///////////////////////////////////////////////////////////
// update_cached_size
//
// Update unode fields about sizes
// NOTE: It does not change inode->i_size
///////////////////////////////////////////////////////////
static inline void
update_cached_size(
    IN usuper         *sbi,
    IN unode          *u,
    IN loff_t         i_size,
    IN const loff_t*  asize
    )
{
  unsigned long flags;

  write_lock_irqsave( &u->rwlock, flags );

  if ( NULL != asize )
    inode_set_bytes( &u->i, *asize );
  if ( u->valid > i_size )
    u->valid  = i_size;
#if 0
  else if ( u->valid < i_size ){
    const char *cur = current->comm;
    if ( 'v' == cur[0] && 'o' == cur[1] && 'l' == cur[2]  && 'd' == cur[3] ) {
      //
      // Emulate 'fallocate' in "vold" when it creates image
      // compare system/vold/Loop.cpp 'int Loop::createImageFile' vs 'int Loop::resizeImageFile'
      //
      u->valid  = i_size;
    }
  }
#endif

  if ( 0 != u->len ) {
    loff_t dvbo = i_size - u->vbo;
    if ( dvbo <= 0 ) {
      u->vbo  = u->lbo  = u->len  = 0;
    } else if ( dvbo < u->len ) {
      u->len  = ((i_size + sbi->cluster_mask ) & sbi->cluster_mask_inv) - u->vbo;
    }
  }

  write_unlock_irqrestore( &u->rwlock, flags );
}


///////////////////////////////////////////////////////////
// ufsd_set_size_hlp
//
// Helper function to truncate/expand file
///////////////////////////////////////////////////////////
static int
ufsd_set_size_hlp(
    IN struct inode *i,
    IN UINT64 old_size,
    IN UINT64 new_size
    )
{
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  int err;

  lock_ufsd( sbi );

  //
  // Call UFSD library
  //
#ifdef UFSD_NTFS
  //
  // 'sb->s_maxbytes' for NTFS is set to MAX_LFS_FILESIZE due to sparse or compressed files
  // the maximum size of normal files is stored in 'sbi->maxbytes'
  //
  if ( !is_sparsed_or_compressed( u ) && new_size > sbi->maxbytes ) {
    err = -EFBIG;
  } else
#endif
  {
    UINT64 allocated;
    err = ufsdapi_file_set_size( u->ufile, new_size, NULL, &allocated );
    if ( 0 == err )
      update_cached_size( sbi, u, new_size, &allocated );
  }

  unlock_ufsd( sbi );

  if ( 0 != err )
    i_size_write( i, old_size );  // Restore inode size if error
  else if ( new_size > old_size )
    truncate_setsize( i, new_size );  // Change in-memory size after ufsd

  return err;
}


typedef struct mapinfo{
  UINT64    lbo;    // Logical byte offset
  UINT64    len;    // Length of map in bytes
  size_t    flags;  // Properties of fragment
} mapinfo;


///////////////////////////////////////////////////////////
// vbo_to_lbo
//
// Maps block to read/write
//  sbi - superblock pointer
//  u - unode pointer
//  vbo - starting virtual byte offset to get vbo->lbo
//    mapping
//  length - length of fragment to be mapped (in bytes)
//  map - pointer to struct where mapping will be saved
//
// Returns 0 on success, and non-zero on error.
///////////////////////////////////////////////////////////
static int
vbo_to_lbo(
    IN usuper   *sbi,
    IN unode    *u,
    IN loff_t   vbo,
    IN loff_t   length,
    OUT mapinfo *map
    )
{
  int err;
  unsigned long flags;
  loff_t dvbo;
  mapinfo2 map2;

  //
  // Check cached info
  //
  read_lock_irqsave( &u->rwlock, flags );

  dvbo = vbo - u->vbo;
  if ( 0 <= dvbo && dvbo <= u->len ) {
    map->len = u->len - dvbo;
    if ( is_lbo_ok( u->lbo ) ) {
      map->lbo = u->lbo + dvbo;
    } else {
      map->lbo = u->lbo;
      if ( 0 != length )
        map->len = 0;
    }
  } else {
    map->len = 0;
  }

  read_unlock_irqrestore( &u->rwlock, flags );

  if ( map->len > 0 && map->len >= length ) {
//    DebugTrace( 0, Dbg, ("vbo_to_lbo (cache) r=%lx, o=%llx, sz=%llx,%llx,%llx  => %llx + %llx", u->i.i_ino, vbo, u->valid, u->i.i_size, u->vbo + map->len, map->lbo, map->len ));

    if ( is_lbo_ok( map->lbo )
      && ( map->lbo >= sbi->dev_size || (map->lbo + map->len) > sbi->dev_size ) ) {
      ufsd_printk( sbi->sb, "vbo_to_lbo (cache): r=%lx, o=%llx, sz=%llx,%llx: lbo %llx + %llx >= dev_size %llx",
                   u->i.i_ino, vbo, u->valid, u->i.i_size, map->lbo, map->len, sbi->dev_size );
      BUG_ON( 1 );
    }

    map->flags = 0;
    return 0;
  }

  lock_ufsd( sbi );

  // At this point, unode *u may be closed (1), but vbo_to_lbo()
  // can be called during writepages, and it needs opened unode.
  // (1): it may be closed in ufsd_file_release() in if branch for NTFS & HFS
  //      (when number of readers and writers == 0) by the call to
  //      ufsdapi_file_close().
  if ( likely( 0 == ( err = ufsdapi_file_map( u->ufile, vbo, length, 0 == length? 0 : UFSD_MAP_VBO_CREATE, &map2 ) ) ) ) {

    unsigned long flags;

    write_lock_irqsave( &u->rwlock, flags );
    u->vbo = vbo - map2.head;
    u->lbo = is_lbo_ok( map2.lbo )? (map2.lbo - map2.head) : map2.lbo;
    u->len = map2.len + map2.head;
    inode_set_bytes( &u->i, map2.alloc );
#ifdef UFSD_NTFS
    u->total_alloc = map2.total_alloc;
#endif
    write_unlock_irqrestore( &u->rwlock, flags );

    map->lbo    = map2.lbo;
    map->len    = map2.len;
    map->flags  = map2.flags;

    if ( is_lbo_ok( map->lbo )
      && ( map->lbo >= sbi->dev_size || (map->lbo + map->len) > sbi->dev_size ) ) {
      ufsd_printk( sbi->sb, "vbo_to_lbo (on-disk): r=%lx, o=%llx, sz=%llx,%llx: lbo %llx + %llx >= dev_size %llx",
                   u->i.i_ino, vbo, u->valid, u->i.i_size, map->lbo, map->len, sbi->dev_size );
      BUG_ON( 1 );
    }

//    DebugTrace( 0, Dbg, ("vbo_to_lbo r=%lx, o=%llx, sz=%llx,%llx,%llx  => %llx + %llx", u->i.i_ino, vbo, u->valid, u->i.i_size, u->vbo + map->len, map->lbo, map->len ));
  } else {
    map->len = 0;
  }

  unlock_ufsd( sbi );

  return err;
}


//
// Parameter structure for
// iget5 call to be passed as 'opaque'.
//

typedef struct ufsd_iget5_param {
  ucreate             *Create;
  finfo               *fi;
  ufsd_file           *fh;
  unsigned int        subdir_count;
  const unsigned char *name;
  size_t              name_len;
} ufsd_iget5_param;


#if is_decl( READDIR_V1 )

  #define READDIR_DECLARE_ARG struct file *file, void *dirent, filldir_t filldir
  #define READDIR_POS         file->f_pos
  #define READDIR_FILL(Name, NameLen, pos, ino, dt) filldir( dirent, Name, NameLen, pos, ino, dt )
  #define iterate             readdir

#elif is_decl( READDIR_V2 )

  #define READDIR_DECLARE_ARG struct file *file, struct dir_context *ctx
  #define READDIR_POS         ctx->pos
  #define READDIR_FILL(Name, NameLen, dpos, ino, dt) (ctx->pos=dpos, !dir_emit( ctx, Name, NameLen, ino, dt ))

#endif


///////////////////////////////////////////////////////////
// ufsd_readdir
//
// file_operations::readdir
//
// This routine is a callback used to fill readdir() buffer.
//  file - Directory pointer.
//    'f_pos' member contains position to start scan from.
//
//  dirent, filldir - data to be passed to
//    'filldir()' helper
///////////////////////////////////////////////////////////
static int
ufsd_readdir(
    READDIR_DECLARE_ARG
    )
{
  struct inode *i = file_inode( file );
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  UINT64 pos      = READDIR_POS;
  ufsd_search *DirScan = NULL;
#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
  size_t cnt = 0;
#endif

  if ( pos >= sbi->end_of_dir ) {
    DebugTrace( 0, Dbg, ("readdir: r=%lx,%llx -> no more", i->i_ino, pos ));
    return 0;
  }

  DebugTrace( +1, Dbg, ("readdir: %p, r=%lx, %llx", file, i->i_ino, pos ));

#ifdef UFSD_EMULATE_DOTS
  if ( 0 == pos ) {
    if ( READDIR_FILL( ".", 1, 0, i->i_ino, DT_DIR ) )
      goto out;
    pos = 1;
  }

  if ( 1 == pos ) {
    if ( READDIR_FILL( "..", 2, 1, parent_ino( file->f_path.dentry ), DT_DIR ) )
      goto out;
    pos = 2;
  }
#endif

  lock_ufsd( sbi );
  assert( NULL != u->ufile );

  if ( 0 == ufsdapi_find_open( sbi->ufsd, u->ufile, pos, &DirScan ) ) {

    ufsd_direntry de;

    //
    // Enumerate UFSD's direntries
    //
    while ( 0 == ufsdapi_find_get( DirScan, &pos, &de ) ) {

      unsigned dt;
      int fd;

#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
      if ( ++cnt > UFSD_EMULATE_SMALL_READDIR_BUFFER )
        break;
#endif

      //
      // Unfortunately nfsd callback function opens file which in turn calls 'lock_ufsd'
      // Linux's mutex does not allow recursive locks
      //
      if ( FlagOn( de.attrib, UFSDAPI_UGM ) ) {
        if ( S_ISREG( de.mode ) )
          dt = DT_REG;
        else if ( S_ISDIR( de.mode ) )
          dt = DT_DIR;
        else if ( S_ISCHR( de.mode ) )
          dt = DT_CHR;
        else if ( S_ISBLK( de.mode ) )
          dt = DT_BLK;
        else if ( S_ISFIFO( de.mode ) )
          dt = DT_FIFO;
        else if ( S_ISSOCK( de.mode ) )
          dt = DT_SOCK;
        else if ( S_ISLNK( de.mode ) )
          dt = DT_LNK;
        else
          dt = DT_REG;
      } else if ( FlagOn( de.attrib, UFSDAPI_SUBDIR ) )
        dt = DT_DIR;
      else
        dt = DT_REG;

      fd = READDIR_FILL( de.name, de.namelen, pos, (ino_t)de.ino, dt );

      if ( fd )
        break;
    }

    ufsdapi_find_close( DirScan );
  }

  unlock_ufsd( sbi );

#ifdef UFSD_EMULATE_DOTS
out:
#endif
  //
  // Save position and return
  //
  READDIR_POS = pos;

  DebugTrace( -1, Dbg, ("readdir -> 0 (next=%llx)", pos));
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_preopen_file
//
// helper function
///////////////////////////////////////////////////////////
static int
ufsd_preopen_file(
    IN usuper *sbi,
    IN unode  *u
    )
{
  int err = 0;

  if ( NULL == u->ufile ) {
    lock_ufsd( sbi );
    err = ufsd_open_by_id( sbi, &u->i );
    unlock_ufsd( sbi );
    if ( err )
      return err;
  }

  if ( test_and_clear_bit( UFSD_UNODE_FLAG_LAZY_INIT_BIT, &u->flags ) && NULL != u->ufile) {
    mapinfo2 map;
    lock_ufsd( sbi );
    if ( likely( 0 == ( err = ufsdapi_file_map( u->ufile, 0, 0, 0, &map ) ) ) ) {
      unsigned long flags;
      write_lock_irqsave( &u->rwlock, flags );
      u->vbo = 0;
      u->lbo = map.lbo;
      u->len = map.len;
      write_unlock_irqrestore( &u->rwlock, flags );
    }
    unlock_ufsd( sbi );
  }

  return err;
}


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// is_stream
//
// Helper function returns non zero if filesystem supports streams and
// 'file' is stream handler
///////////////////////////////////////////////////////////
static inline const unsigned char*
is_stream(
    IN struct file *file
    )
{
  const unsigned char *r = file->private_data;
#if 0
  // Nobody should use 'file->private_data'
  return r;
#else
  // Safe check
  if ( NULL != r ) {
    int d = (int)(r - file_dentry(file)->d_name.name);
    if ( 0 < d && d <= file_dentry(file)->d_name.len )
      return r;
    assert( 0 );
  }
  return NULL;
#endif
}
#else
#define is_stream( file ) 0
#endif

#if is_struct( KIOCB_KI_FLAGS ) && defined IOCB_DIRECT && IOCB_DIRECT && defined IOCB_APPEND && IOCB_APPEND
  #define IS_IO_DIRECT( __IOCB, __FILE )  FlagOn( __IOCB->ki_flags, IOCB_DIRECT )
  #define IS_IO_APPEND( __IOCB, __FILE )  FlagOn( __IOCB->ki_flags, IOCB_APPEND )
#else
  #define IS_IO_DIRECT( __IOCB, __FILE )  FlagOn( __FILE->f_flags, O_DIRECT )
  #define IS_IO_APPEND( __IOCB, __FILE )  FlagOn( __FILE->f_flags, O_APPEND )
#endif

#if is_decl( INODE_TRYLOCK )
#define Inode_is_locked(_i)  inode_is_locked(_i)
#define Inode_trylock(_i)    inode_trylock(_i)
#define Inode_unlock(_i)     inode_unlock(_i)
#define Inode_lock(_i)       inode_lock(_i)
#else // helpers for inode locks in case of missing, 4.6-
static inline int  Inode_is_locked(struct inode *i) { return mutex_is_locked(&i->i_mutex); }
static inline int  Inode_trylock(struct inode *i)   { return mutex_trylock(&i->i_mutex); }
static inline void Inode_unlock(struct inode *i)    { mutex_unlock(&i->i_mutex); }
static inline void Inode_lock(struct inode *i)      { mutex_lock(&i->i_mutex); }
#endif


///////////////////////////////////////////////////////////
// ufsd_file_open
//
// file_operations::open
///////////////////////////////////////////////////////////
static int
ufsd_file_open(
    IN struct inode *i,
    IN struct file  *file
    )
{
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  TRACE_ONLY( struct qstr *s  = &file_dentry(file)->d_name; )
  TRACE_ONLY( const char *hint=""; )
  int err;

  assert( file->f_mapping == i->i_mapping && "Check kernel config!" );
  VfsTrace( +1, Dbg, ("file_open: r=%lx, l=%x, f=%p, fl=o%o%s%s, '%.*s'",
                i->i_ino, i->i_nlink, file, file->f_flags,
                FlagOn( file->f_flags, O_DIRECT )?",d":"", FlagOn( file->f_flags, O_APPEND )?",a":"",
                (int)s->len, s->name ));

  // increment before 'ufsd_preopen_file'
  atomic_inc( &u->i_opencount );

  // Check file size
  err = generic_file_open( i, file );
  if ( likely( 0 == err ) )
    err = ufsd_preopen_file( sbi, u );

  if ( unlikely( 0 != err ) ) {
    TRACE_ONLY( hint="failed"; )
  } else if ( unlikely( ( is_compressed( u ) || is_encrypted( u ) ) && FlagOn( file->f_flags, O_DIRECT ) ) ) {
    TRACE_ONLY( hint="failed to open compressed file with O_DIRECT"; )
    err = -ENOTBLK;
  }

  if ( unlikely( 0 != err ) ) {
    VfsTrace( -1, Dbg, ("file_open -> %s, %d", hint, err));
    atomic_dec( &u->i_opencount );
    return err;
  }

#ifdef UFSD_NTFS
  assert( NULL == file->private_data );
  if ( 0 != sbi->options.delim ) {
#ifndef UFSD_TRACE
    struct qstr *s  = &file_dentry(file)->d_name;
#endif
    char *p = strchr( s->name, sbi->options.delim );
    if ( NULL != p ) {
      igrab( i );
      dget( file_dentry(file) );
      file->private_data = p + 1;
      assert( is_stream( file ) );
      TRACE_ONLY( hint="(stream)"; )
    }
  }
#endif

  VfsTrace( -1, Dbg, ("file_open%s -> ok%s, sz=%llx,%llx", hint, is_compressed( u )?", c" : "", u->valid, i->i_size ));

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_update_ondisk
//
// Stores information from inode into ufsd_sdk format
///////////////////////////////////////////////////////////
static unsigned
ufsd_update_ondisk(
    IN usuper       *sbi,
    IN struct inode *i,
    OUT finfo       *fi
    )
{
  unode *u  = UFSD_U( i );

  if ( u->stored_noacsr ) {
    fi->Uid   = __kuid_val( u->i_uid );
    fi->Gid   = __kgid_val( u->i_gid );
    fi->Mode  = u->i_mode;
  } else {
    fi->Uid   = __kuid_val( i->i_uid );
    fi->Gid   = __kgid_val( i->i_gid );
    fi->Mode  = i->i_mode;
  }

#if 0
  DebugTrace( 0, Dbg, ("update_ondisk (r=%lx): m=%o, t=%lx+%lu, %lx+%lu, %lx+%lu, s=%llx",
                      i->i_ino, (unsigned)i->i_mode,
                      i->i_atime.tv_sec, i->i_atime.tv_nsec,
                      i->i_mtime.tv_sec, i->i_mtime.tv_nsec,
                      i->i_ctime.tv_sec, i->i_ctime.tv_nsec,
                      i->i_size ));
#endif

  fi->ReffTime    = kernel2ufsd( sbi, &i->i_atime );
  fi->ChangeTime  = kernel2ufsd( sbi, &i->i_ctime );
  fi->ModiffTime  = kernel2ufsd( sbi, &i->i_mtime );

  if ( is_compressed( u ) )
    return ATTR_UID | ATTR_GID | ATTR_MODE | ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;

  //
  // Ask ufsd to update on-disk valid/size
  //
  fi->ValidSize = get_valid_size( u, &fi->FileSize, NULL );
  return ATTR_SIZE | ATTR_UID | ATTR_GID | ATTR_MODE | ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
}


///////////////////////////////////////////////////////////
// ufsd_file_release
//
// file_operations::release
///////////////////////////////////////////////////////////
static int
ufsd_file_release(
    IN struct inode *i,
    IN struct file  *file
    )
{
  TRACE_ONLY( const char *hint=""; )

  unode *u = UFSD_U( i );

  // 'release' should be called for each success 'open'
  assert( atomic_read( &u->i_opencount ) >= 1 );

#ifdef UFSD_NTFS
  if ( is_stream( file ) ) {
    dput( file_dentry(file) );
    iput( i );
    TRACE_ONLY( hint="(stream)"; )
  } else
#endif
  {
    usuper *sbi = UFSD_SB( i->i_sb );
    UNREFERENCED_PARAMETER( sbi );

#if 1
    // if we are the last writer on the inode, drop the block reservation
    if ( FlagOn( file->f_mode, FMODE_WRITE )
      && 1 == atomic_read( &i->i_writecount ) ) {

      UINT64 allocated;
      Inode_lock( i );
      lock_ufsd( sbi );

      ufsdapi_file_flush( sbi->ufsd, u->ufile, sbi->fi, ufsd_update_ondisk( sbi, i, sbi->fi ), i, 0, &allocated );
      if ( !is_compressed( u ) )
        update_cached_size( sbi, u, i->i_size, &allocated );

      spin_lock( &i->i_lock );
      i->i_state &= ~(I_DIRTY_SYNC | I_DIRTY_DATASYNC);
      spin_unlock( &i->i_lock );

      unlock_ufsd( sbi );
      Inode_unlock( i );

      TRACE_ONLY( hint="(updated)"; )
    }
#endif

#if ( defined UFSD_NTFS || defined UFSD_HFS ) && defined UFSD_CLOSE_AT_RELEASE
    //
    // Free resources if last handle closed for directories on NTFS & HFS.
    //
    if ( ( is_ntfs( &sbi->options ) || is_hfs( &sbi->options ) )
        && S_ISDIR( i->i_mode ) && 1 == atomic_read( &u->i_opencount ) ) {

      ufsd_file *fh;
      spin_lock( &i->i_lock );
      fh = u->ufile;
      u->ufile = NULL;
      spin_unlock( &i->i_lock );
      assert( NULL != fh );
      if ( NULL != fh ) {
        lock_ufsd( sbi );
        ufsdapi_file_close( sbi->ufsd, fh );
        unlock_ufsd( sbi );
      }

      TRACE_ONLY( hint="(closed)"; )
    }
#endif
  }

  atomic_dec( &u->i_opencount );

  VfsTrace( 0, Dbg, ("file_release%s: -> r=%lx, f=%p, z=%d",
        hint, i->i_ino, file, atomic_read( &u->i_opencount ) ));

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_file_fsync
//
// file_operations::fsync
///////////////////////////////////////////////////////////
static int
ufsd_file_fsync(
    IN struct file *file,
#if is_decl( FO_FSYNC_V1 )
    IN struct dentry *de,
#elif is_decl( FO_FSYNC_V2 )
#elif is_decl( FO_FSYNC_V3 )
    IN loff_t start,
    IN loff_t end,
#endif
    IN int datasync
    )
{
  struct inode *i = file_inode( file );
  struct super_block *sb = i->i_sb;
  int err;

  if ( unlikely( !is_bdi_ok( sb ) ) )
    return -ENODEV;

  VfsTrace( +1, Dbg, ("fsync: r=%lx,%lx %d", i->i_ino, i->i_state, datasync ));

#if is_decl( FO_FSYNC_V3 )
  err = filemap_write_and_wait_range( i->i_mapping, start, end );
#else
  err = filemap_write_and_wait_range( i->i_mapping, 0, -1 );
#endif

  if ( likely( 0 == err ) ) {
    unsigned long i_state;
    spin_lock( &i->i_lock );
    i_state = i->i_state;
    spin_unlock( &i->i_lock );

    if ( FlagOn( i_state, I_DIRTY ) && ( !datasync || FlagOn( i_state, I_DIRTY_DATASYNC ) ) ) {
      struct writeback_control wbc = {
        .sync_mode    = WB_SYNC_ALL,
        .nr_to_write  = 0, // metadata-only
      };
      err = sync_inode( i, &wbc );
//      err = sync_inode_metadata( i, 1 ); // 2.6.37+
    }
  }

  // We called flush in sync_inode => ufsd_write_inode, that
  // marks file's bh dirty with ufsd_bd_write, so sync_blockdev can see them
  sync_blockdev( sb->s_bdev );

  ufsd_bd_barrier( sb );

  VfsTrace( -1, Dbg, ("fsync => %d,%lx", err, i->i_state ));
  return err;
}


#ifndef UFSD_NO_USE_IOCTL

#ifndef VFAT_IOCTL_GET_VOLUME_ID
  #define VFAT_IOCTL_GET_VOLUME_ID  _IOR('r', 0x12, __u32)
#endif

///////////////////////////////////////////////////////////
// ufsd_ioctl
//
// file_operations::ioctl
///////////////////////////////////////////////////////////
static long
ufsd_ioctl(
    IN struct file    *file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  struct inode *i = file_inode( file );
  struct super_block *sb = i->i_sb;
  int err;
  unsigned ioctl  = _IOC_NR( cmd );
  unsigned insize = _IOC_DIR( cmd ) & _IOC_WRITE? _IOC_SIZE( cmd ) : 0;
  unsigned osize  = _IOC_DIR( cmd ) & _IOC_READ? _IOC_SIZE( cmd ) : 0;
  void* final_buffer = NULL;
  size_t BytesReturned;
  usuper *sbi  = UFSD_SB( sb );
  finfo *fi;
  unode *u = UFSD_U( i );
#if defined FITRIM
  char discard = sbi->options.discard;
#endif

  VfsTrace( +1, Dbg,("ioctl: ('%.*s'), r=%lx, m=%o, f=%p, %08x, %lx",
                       (int)file_dentry(file)->d_name.len, file_dentry(file)->d_name.name,
                       i->i_ino, i->i_mode, file, cmd, arg));

  if ( VFAT_IOCTL_GET_VOLUME_ID == cmd ) {
    //
    // Special code
    //
    err = ufsdapi_query_volume_id( sbi->ufsd );
    VfsTrace( -1, Dbg, ("ioctl (VFAT_IOCTL_GET_VOLUME_ID ) -> %x", (unsigned)err));
    return err;
  }

  switch( cmd ) {
  case UFSD_IOC_GETSIZES:
  case UFSD_IOC_SETVALID:
  case UFSD_IOC_SETCLUMP:
  case UFSD_IOC_SETTIMES:
  case UFSD_IOC_GETTIMES:
  case UFSD_IOC_SETATTR:
  case UFSD_IOC_GETATTR:
  case UFSD_IOC_GETMEMUSE:
  case UFSD_IOC_GETVOLINFO:
    break;

#if defined FITRIM
  case FITRIM:
    // allow discard requests
    sbi->options.discard = 1;

    if ( !capable( CAP_SYS_ADMIN ) ) {
      err = -EPERM;
      goto out;
    }

    if ( 0 == sbi->discard_granularity ) {
      ufsd_printk( sb, "looks like device \"%s\" does not support trim", sb->s_id );
      err = -EOPNOTSUPP;
      goto out;
    }

    // code 121 is already used by ufsd
    ioctl = 44;   // IOCTL_FS_TRIM_RANGE
    break;
#endif

  default:
    VfsTrace( -1, Dbg, ("ioctl(%x) -> '-ENOTTY'", ioctl));
    return -ENOTTY;
  }

  final_buffer = ufsd_heap_alloc( max_t(unsigned, insize, osize), 0 );
  if ( NULL == final_buffer ) {
    err = -ENOMEM;
  } else {

    if ( copy_from_user( final_buffer, (void*)arg, insize) ) {
      err = -EFAULT;
    } else {

      assert( NULL != i );
      assert( NULL != UFSD_VOLUME(sb) );

      if ( UFSD_IOC_GETSIZES == cmd ) {
        loff_t* s = (loff_t*)final_buffer;

        s[2] = get_valid_size( u, s+1, s+0 );
#ifdef UFSD_NTFS
        s[3] = is_sparsed_or_compressed( u )? u->total_alloc : 0; // The sum of the allocated clusters for a compressed file
#else
        s[3] = 0;
#endif
        err = 0;
      } else {

        //
        // And call the library.
        //
        lock_ufsd( sbi );

        err = ufsdapi_ioctl( sbi->ufsd, u->ufile, ioctl, final_buffer, insize, osize, &BytesReturned, &fi );

        if ( 0 == err ) {
          switch( cmd ) {
          case UFSD_IOC_SETTIMES:
            ufsd_times_to_inode( sbi, fi, i );
            mark_inode_dirty( i );
            break;
          case UFSD_IOC_SETVALID:
            set_valid_size( u, fi->ValidSize );
            // no break here!
          case UFSD_IOC_SETATTR:
            mark_inode_dirty( i );
            break;
          }
        } else if ( -ERANGE == err ) {
          err = -EOVERFLOW;
        }

        unlock_ufsd( sbi );
      }
      if ( copy_to_user( (void *)arg, final_buffer, osize ) )
        err = -EFAULT;
    }

    ufsd_heap_free( final_buffer );
  }

#if defined FITRIM
  if ( FITRIM == cmd ) {
out:
    // restore mount option "discard"
    sbi->options.discard = discard;
  }
#endif

  VfsTrace( -1, Dbg, ("ioctl(%x) -> %d", ioctl, err));
  return err;
}


#ifdef CONFIG_COMPAT
#include <linux/compat.h>
///////////////////////////////////////////////////////////
// ufsd_compat_ioctl
//
// 32 application -> 64 bit driver
///////////////////////////////////////////////////////////
static long
ufsd_compat_ioctl(
    IN struct file    *file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  switch( cmd ) {
  case UFSD_IOC32_GETMEMUSE:
    cmd = UFSD_IOC_GETMEMUSE;
    break;
  }

  return ufsd_ioctl( file, cmd, (unsigned long)compat_ptr(arg) );
}
#endif // #ifdef CONFIG_COMPAT
#endif // #ifndef UFSD_NO_USE_IOCTL

static const struct file_operations ufsd_dir_operations = {
  .llseek   = generic_file_llseek,
  .read     = generic_read_dir,
  .iterate  = ufsd_readdir,
  .fsync    = ufsd_file_fsync,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
#ifndef UFSD_NO_USE_IOCTL
  .unlocked_ioctl = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = ufsd_compat_ioctl,
#endif
#endif
};


///////////////////////////////////////////////////////////
// ufsd_d_hash
//
// dentry_operations::d_hash
///////////////////////////////////////////////////////////
static int
ufsd_d_hash(
#if is_decl( DHASH_V1 )
    IN struct dentry *de,
#else
    IN const struct dentry *de,
#endif
#if is_decl( DHASH_V2 )
    IN const struct inode  *i,
#endif
    IN struct qstr         *name
    )
{
  usuper* sbi       = UFSD_SB( de->d_sb );
  const char* n     = name->name;
  unsigned int len  = name->len;
  unsigned int c;
  unsigned long hash;
  DEBUG_ONLY( sbi->nHashCalls += 1; )

#ifdef UFSD_HFS
  if ( !sbi->options.nocase ) {
    assert( is_hfs( &sbi->options ) );
    for ( ;; ) {
      if ( 0 == len-- )
        return 0; // as is

      c = *n++;
      if ( c >= 0x80 )
        break;
    }
  } else
#endif
  {
    hash  = (unsigned long)de; // init_name_hash( de );
    for ( ;; ) {
      if ( 0 == len-- ) {
        hash = end_name_hash( hash );
        goto out;
      }

      c = *n++;
      if ( c >= 0x80 )
        break;

      hash = partial_name_hash( tolower( c ), hash );
    }
  }

  DEBUG_ONLY( sbi->nHashCallsUfsd += 1; )

  spin_lock( &sbi->nocase_lock );
  hash = ufsdapi_names_hash( sbi->ufsd, name->name, name->len );
  spin_unlock( &sbi->nocase_lock );

out:
  name->hash = hash;
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_d_compare
//
// dentry_operations::d_compare
// return 0 if names match
///////////////////////////////////////////////////////////
static int
ufsd_d_compare(
#if is_decl( DCOMPARE_V1 )
    IN struct dentry *de,
    IN struct qstr   *name1,
    IN struct qstr   *name
#else
    IN const struct dentry *de,
#if is_decl( DCOMPARE_V2 )
    IN const struct inode  *iparent,
#endif
#if is_decl( DCOMPARE_V2 ) || is_decl( DCOMPARE_V3 )
    IN const struct dentry *de2,
#endif
#if is_decl( DCOMPARE_V2 )
    IN const struct inode  *i,
#endif
    IN unsigned int         len1,
    IN const char          *str,
    IN const struct qstr   *name
#endif
    )
{
  int ret;
#if is_decl( DCOMPARE_V1 )
  const char *str   = name1->name;
  unsigned int len1 = name1->len;
#endif
  usuper* sbi       = UFSD_SB( de->d_sb );
  const char *n1    = str;
  const char *n2    = name->name;
  unsigned int len2 = name->len;
  unsigned int lm   = min( len1, len2 );
  unsigned char c1, c2;
  DEBUG_ONLY( sbi->nCompareCalls += 1; )

#ifdef UFSD_HFS
  if ( !sbi->options.nocase ) {
    assert( is_hfs( &sbi->options ) );
    for ( ;; ) {
      if ( 0 == lm-- ) {
        ret = len1 == len2? 0 : 1;
        goto out;
      }

      if ( (c1 = *n1++) == (c2 = *n2++) )
        continue;

      if ( c1 >= 0x80 || c2 >= 0x80 )
        break;

      ret = 1;
      goto out;
    }
  } else
#endif
  {
    for ( ;; ) {
      if ( 0 == lm-- ) {
        ret = len1 == len2? 0 : 1;
        goto out;
      }

      if ( (c1 = *n1++) == (c2 = *n2++) )
        continue;

      if ( c1 >= 0x80 || c2 >= 0x80 )
        break;

      if ( tolower( c1 ) != tolower( c2 ) ) {
        ret = 1;
        goto out;
      }
    }
  }

  DEBUG_ONLY( sbi->nCompareCallsUfsd += 1; )

  spin_lock( &sbi->nocase_lock );
  ret = 1 == ufsdapi_names_equal( sbi->ufsd, str, len1, name->name, len2 )? 0 : 1;
  spin_unlock( &sbi->nocase_lock );

out:
  return ret;
}


static struct dentry_operations ufsd_dop = {
  .d_hash     = ufsd_d_hash,
  .d_compare  = ufsd_d_compare,
};


// Forward declaration
static struct inode*
ufsd_create_or_open (
    IN struct inode       *dir,
    IN OUT struct dentry  *de,
    IN ucreate            *cr
    );

#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
static int
ufsd_acl_chmod(
    IN struct inode *i
    );
#endif

#if is_decl( INOP_CREATE_V3 ) || is_decl( INOP_CREATE_V4 )
  typedef umode_t  Umode_t;
#else
  typedef int      Umode_t;
#endif

///////////////////////////////////////////////////////////
// ufsd_create
//
// create/open use the same helper.
// inode_operations::create
///////////////////////////////////////////////////////////
static int
ufsd_create(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t         mode
#if is_decl( INOP_CREATE_V2 ) || is_decl( INOP_CREATE_V3 )
    , struct nameidata *nd
#elif is_decl( INOP_CREATE_V4 )
    , bool namei
#endif
    )
{
  struct inode *i;
  ucreate  cr = { NULL, NULL, 0, 0, 0, mode };

  if ( IS_ERR( i = ufsd_create_or_open( dir, de, &cr ) ) )
    return PTR_ERR( i );

  d_instantiate( de, i );

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_mkdir
//
// inode_operations::mkdir
///////////////////////////////////////////////////////////
static int
ufsd_mkdir(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode
    )
{
  struct inode *i;
  ucreate  cr = { NULL, NULL, 0, 0, 0, mode | S_IFDIR };

  if ( IS_ERR( i = ufsd_create_or_open( dir, de, &cr ) ) )
    return PTR_ERR( i );

  d_instantiate( de, i );

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_unlink
//
// inode_operations::unlink
// inode_operations::rmdir
///////////////////////////////////////////////////////////
static int
ufsd_unlink(
    IN struct inode   *dir,
    IN struct dentry  *de
    )
{
  int err;
  UINT64 dir_size   = 0;
  struct inode *i   = de->d_inode;
  usuper *sbi       = UFSD_SB( i->i_sb );
  struct qstr *s    = &de->d_name;
  unode  *u         = UFSD_U( i );
#ifdef UFSD_NTFS
  unsigned char *p  = 0 == sbi->options.delim? NULL : strchr( s->name, sbi->options.delim );
  const char *sname;
  int flen, slen;

  if ( NULL == p ) {
    flen  = s->len;
    sname = NULL;
    slen  = 0;
  } else {
    flen  = p - s->name;
    sname = p + 1;
    slen  = s->name + s->len - p - 1;
  }
#else
  int flen = s->len;
  const char *sname = NULL;
  int slen = 0;
#endif

  VfsTrace( +1, Dbg, ("unlink: r=%lx, ('%.*s'), r=%lx, l=%x,%d, de=%p",
              dir->i_ino, (int)s->len, s->name, i->i_ino, i->i_nlink, atomic_read( &u->i_opencount ), de ));

  assert( 0 != i->i_nlink );

  if ( unlikely( is_fork( u ) ) ) {
    err = -EPERM;
    goto out;
  }

  lock_ufsd( sbi );

  // ufsdapi_unlink() requires ufsd object to be opened; make sure that it is
  // opened (1).
  // (1): it may be closed in ufsd_file_release() in if branch for NTFS & HFS
  //      (when number of readers and writers == 0) by the call to
  //      ufsdapi_file_close().
  err = ufsd_open_by_id( sbi, dir );
  if ( likely( 0 == err ) ) {
    // If file is not opened, it is assumed that hard-link count for that file
    // is 1. But it may happen, that real hard-link count value is > 1. When
    // we unlink such file, we decrement hard-link count (it becomes 0), but
    // there are other directory entries point to the same inode (MFT).
    // unlink()'ing all entries for such file lead to wrong hard-link count
    // (it gets decremented from 1 to 0, and then from 0 it underflows to
    // 0xffffffff).
    //
    // Fix this by ufsd_open_by_id()'ing the file.
    err = ufsd_open_by_id( sbi, i );
    if ( likely( 0 == err ) ) {
      err = ufsdapi_unlink( sbi->ufsd, UFSD_FH( dir ), s->name, flen, sname, slen,
                            1 == i->i_nlink && 0 == atomic_read( &u->i_opencount ),
                            &u->ufile, &dir_size );
      if ( 0 == err && NULL == u->ufile ) {
        i_size_write( i, 0 );
        DebugTrace( 0, Dbg, ("unlink immediate r=%lx", i->i_ino ));
      }
    }
  }

  unlock_ufsd( sbi );

  if ( unlikely( 0 != err ) ) {
    switch( err ) {
    case -ENOTEMPTY:
    case -ENOSPC:
      break;
    default:
      make_bad_inode( i );
    }
    goto out;
  }

#ifdef UFSD_NTFS
  if ( NULL == sname )
#endif
  {
    drop_nlink( i );

    // Mark dir as requiring resync.
    dir->i_mtime = dir->i_ctime = ufsd_inode_current_time( sbi );
    i_size_write( dir, dir_size );
    inode_set_bytes( dir, dir_size );
    mark_inode_dirty( dir );
    i->i_ctime    = dir->i_ctime;
    if ( i->i_nlink )
      mark_inode_dirty( i );
  }

#ifdef UFSD_COUNT_CONTAINED
  if ( S_ISDIR( i->i_mode ) ) {
    assert(dir->i_nlink > 0);
    drop_nlink( dir );
    mark_inode_dirty( dir );
  }
#endif

out:
  VfsTrace( -1, Dbg, ("unlink -> %d", err));
  return err;
}


#ifdef UFSD_USE_HFS_FORK

static int ufsd_set_inode( IN struct inode *i, IN ufsd_iget5_param *p );

///////////////////////////////////////////////////////////
// ufsd_file_lookup
//
// inode_operations::lookup
///////////////////////////////////////////////////////////
static struct dentry *
ufsd_file_lookup(
    IN struct inode   *dir,
    IN struct dentry  *de
#if is_decl( INOP_LOOKUP_V2 )
    , IN struct nameidata *nd
#elif is_decl( INOP_LOOKUP_V3 )
    , IN unsigned int nd
#endif
    )
{
  ufsd_iget5_param param;
  struct super_block *sb  = dir->i_sb;
  usuper  *sbi            = UFSD_SB( sb );
  unode *udir             = UFSD_U( dir );
  struct inode *i         = udir->fork_inode;
  struct dentry *ret;
  unode   *u;
  int err;

  //
  // 'dir' is a file inode
  //
  assert( !S_ISDIR( dir->i_mode ) );

  DebugTrace( +1, Dbg, ("file_lookup: r=%lx, '%s'", dir->i_ino, de->d_name.name ));

  if ( unlikely( is_fork( udir ) ) ) {
    ret = ERR_PTR( -EOPNOTSUPP );
    goto out; // resource file does not have resources
  }

  //
  // every hfs file has fork "resource"
  // macosx allows to operate with forks via '/rsrc'
  //
  if ( !sbi->options.hfs || strcmp( de->d_name.name, "rsrc" ) ) {
    ret = ERR_PTR( -ENOTDIR );
    goto out; // only /rsrc appendex allowed
  }

  if ( NULL != i ) {
    TRACE_ONLY( u = UFSD_U( i ); )
    goto ok; // already opened
  }

  i = new_inode( sb );
  if ( NULL == i ) {
    ret = ERR_PTR( -ENOMEM );
    goto out;
  }

  memset( &param, 0, sizeof(param) );
  u   = UFSD_U( i );
  lock_ufsd( sbi );
  err = ufsdapi_file_open_fork( sbi->ufsd, udir->ufile, &param.fh, &param.fi );
  unlock_ufsd( sbi );

  if ( 0 == err ) {
    i->i_ino = dir->i_ino;
    assert( NULL == u->ufile );
    assert( NULL != param.fh );
    ufsd_set_inode( i, &param );
    assert( NULL != u->ufile );
    assert( NULL == param.fh );
    set_bit( UFSD_UNODE_FLAG_FORK_BIT, &u->flags );
    udir->fork_inode  = i;
    u->fork_inode     = dir;
    igrab( dir );

    //
    // hlist_add_fake is added in 2.6.37+
    // It is very simple:
    // static inline void hlist_add_fake(struct hlist_node *n) { n->pprev = &n->next; }
    // Use explicit code to avoid configure
    //
    i->i_hash.pprev = &i->i_hash.next;  // hlist_add_fake( &i->i_hash );
    mark_inode_dirty( i );

ok:
    d_add( de, i );

    DebugTrace( -1, Dbg, ("file_lookup -> ok, base=%p, fork=%p, h=%p", dir, i, u->ufile ));
    return NULL;
  }

  iput( i );
  ret = ERR_PTR( -EACCES );

out:
  DebugTrace( -1, Dbg, ("file_lookup -> %ld", PTR_ERR( ret ) ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_file_create
//
// inode_operations::create
///////////////////////////////////////////////////////////
static int
ufsd_file_create(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t         mode
#if is_decl( INOP_CREATE_V2 ) || is_decl( INOP_CREATE_V3 )
    , struct nameidata *nd
#elif is_decl( INOP_CREATE_V4 )
    , bool namei
#endif
    )
{
  DebugTrace( 0, Dbg, ("file_create -> EINVAL" ) );
  return -EINVAL;
}
#endif // #ifdef UFSD_USE_HFS_FORK


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_block_truncate_page
//
// fs/buffer.c : block_truncate_page
///////////////////////////////////////////////////////////
static int
ufsd_block_truncate_page(
    IN unode *u,
    IN loff_t from
    )
{
  int err = 0;
  struct super_block *sb = u->i.i_sb;
  pgoff_t index       = from >> PAGE_SHIFT;
  unsigned offset     = from & (PAGE_SIZE-1);
  unsigned blocksize  = sb->s_blocksize;
  unsigned blkbits    = sb->s_blocksize_bits;
  unsigned vblock     = offset >> blkbits;
  unsigned bh_off     = vblock << blkbits;
  struct buffer_head *bh;
  struct page *page;
  struct address_space *mapping = u->i.i_mapping;

  page = find_or_create_page( mapping, index, mapping_gfp_mask( mapping ) & ~__GFP_FS );
  if ( NULL == page )
    return -ENOMEM;

  DebugTrace( +1, Dbg, ("block_truncate_page: r=%lx, -> %llx", u->i.i_ino, from ) );

  if ( !page_has_buffers( page ) )
    create_empty_buffers( page, blocksize, 0 );

  // Find the buffer that contains "offset"
  bh  = page_buffers( page );
  while( 0 != vblock-- )
    bh = bh->b_this_page;

  assert( bh_off == bh_offset( bh ) );
  assert( offset >= bh_off && offset < bh_off + blocksize );

  if ( !buffer_mapped( bh ) ) {
    mapinfo map;
    //
    // map to read
    //
    if ( unlikely( vbo_to_lbo( UFSD_SB( sb ), u, ((loff_t)index << PAGE_SHIFT) + bh_off, 0, &map ) ) )
      goto unlock;

    if ( !is_lbo_ok( map.lbo ) || 0 == map.len )
      goto unlock;

    bh->b_bdev    = sb->s_bdev;
    bh->b_blocknr = map.lbo >> blkbits;
    set_buffer_mapped( bh );
    DebugTrace( 0, Dbg, ("set_buffer_mapped - b=%" PSCT "x", bh->b_blocknr ) );
  }

  // Ok, it's mapped. Make sure it's up-to-date
  if ( PageUptodate( page ) )
    set_buffer_uptodate( bh );

  if ( !buffer_uptodate( bh ) ) {
    DebugTrace( 0, Dbg, ("block_truncate_page - read b=%" PSCT "x", bh->b_blocknr ) );
    Ll_rw_block( READ, 0, 1, &bh );
    wait_on_buffer( bh );
    if ( !buffer_uptodate( bh ) ) {
      // Read error
      err = -EIO;
      goto unlock;
    }
  }

  // zero until the end of block
  zero_user_segment( page, offset, bh_off + blocksize );
  mark_buffer_dirty( bh );

unlock:
  unlock_page( page );
  put_page( page );

  DebugTrace( -1, Dbg, ("block_truncate_page -> %d", err ) );
  return err;
}
#endif // #ifdef UFSD_NTFS


///////////////////////////////////////////////////////////
// ufsd_set_size
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_set_size(
    IN struct inode *i,
    IN UINT64 old_size,
    IN UINT64 new_size
    )
{
  int err;
  unode *u  = UFSD_U( i );
  TRACE_ONLY( const char *hint = new_size >= old_size? "expand":"truncate"; )
  unsigned long flags;

  VfsTrace( +1, Dbg, ("%s: r=%lx, sz=%llx,%llx -> %llx%s", hint, i->i_ino, u->valid, old_size, new_size, is_sparsed( u )?" ,sp" : "" ) );

  assert( Inode_is_locked( i ) );

  // If truncate update valid size first
  write_lock_irqsave( &u->rwlock, flags );
  if ( new_size < u->valid )
    u->valid = new_size;
  write_unlock_irqrestore( &u->rwlock, flags );

  if ( new_size < old_size ) {
    // Change in-memory size before ufsd
    truncate_setsize( i, new_size );

#ifdef UFSD_NTFS
    if ( is_sparsed( u ) && (new_size & (i->i_sb->s_blocksize - 1)) )
      ufsd_block_truncate_page( u, new_size );
#endif
  }

  //
  // if expand then 'ufsd_set_size_hlp' calls 'truncate_setsize( i, new_size )'
  //
  err = ufsd_set_size_hlp( i, old_size, new_size );

  if ( unlikely( 0 != err ) ) {
    VfsTrace( -1, Dbg, ("%s failed -> %d", hint, err ) );
    return err;
  }

  VfsTrace( -1, Dbg, ("%s r=%lx, sz=%llx,%llx, ok", hint, i->i_ino, u->valid, i->i_size ) );

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_setattr
//
// inode_operations::setattr
///////////////////////////////////////////////////////////
static int
ufsd_setattr(
    IN struct dentry *de,
    IN struct iattr  *attr
    )
{
  int err;
  struct inode *i = de->d_inode;
#if defined UFSD_NTFS || defined UFSD_TRACE
  unode *u        = UFSD_U( i );
#endif
  usuper *sbi     = UFSD_SB( de->d_sb );
  unsigned int ia_valid = attr->ia_valid;
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )

  VfsTrace( +1, Dbg, ("setattr(%x): r=%lx, uid=%d,gid=%d,m=%o,sz=%llx,%llx",
                        ia_valid, i->i_ino, __kuid_val(i->i_uid), __kgid_val(i->i_gid), i->i_mode,
                        u->valid, i->i_size ));

  if ( sbi->options.no_acs_rules ) {
    // "no access rules" - force any changes of time etc.
    SetFlag( attr->ia_valid, ATTR_FORCE );
    // and disable for editing some attributes
    ClearFlag( attr->ia_valid, UFSD_NOACSR_ATTRS );
    ia_valid = attr->ia_valid;
  }

#if is_decl( INODE_CHANGE_OK_V1 )
  err = inode_change_ok( i, attr );
#elif is_decl( INODE_CHANGE_OK_V2 )
  err = setattr_prepare( de, attr );
#endif

  if ( err ) {
#ifdef UFSD_DEBUG
    unsigned int fs_uid   = __kuid_val( current_fsuid() );
    DebugTrace( 0, Dbg, ("inode_change_ok failed: \"%s\" current_fsuid=%d, ia_valid=%x", current->comm, fs_uid, ia_valid ));
    if ( FlagOn( ia_valid, ATTR_UID ) )
      DebugTrace( 0, Dbg, ("new uid=%d, capable(CAP_CHOWN)=%d", __kuid_val( attr->ia_uid ), capable(CAP_CHOWN) ));

    if ( FlagOn( ia_valid, ATTR_GID ) )
      DebugTrace( 0, Dbg, ("new gid=%d, in_group_p=%d, capable(CAP_CHOWN)=%d", __kgid_val( attr->ia_gid ), in_group_p(attr->ia_gid), capable(CAP_CHOWN) ));

    if ( FlagOn( ia_valid, ATTR_MODE ) )
      DebugTrace( 0, Dbg, ("new mode=%o, inode_owner_or_capable=%d", (unsigned)attr->ia_mode, (int)inode_owner_or_capable(i) ));

#ifndef ATTR_TIMES_SET
  #define ATTR_TIMES_SET  (1 << 16)
#endif
    if ( ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET) )
      DebugTrace( 0, Dbg, ("new times, inode_owner_or_capable=%d", (int)inode_owner_or_capable(i) ));
#endif
    goto out;
  }

  if ( FlagOn( ia_valid, ATTR_SIZE ) ) {
    if ( attr->ia_size == i->i_size )
      ClearFlag( ia_valid, ATTR_SIZE );
    else {
      if ( unlikely( is_encrypted( u ) ) ) {
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("setattr: attempt to resize encrypted file" ) );
        err = -ENOSYS;
        goto out;
      }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
      inode_dio_wait( i );
#endif

      err = ufsd_set_size( i, i->i_size, attr->ia_size );
      if ( 0 != err )
        goto out;
      i->i_mtime = i->i_ctime = ufsd_inode_current_time( sbi );
    }
  }

  //
  // Update inode info
  //
  if ( FlagOn( ia_valid, ATTR_UID ) )
    i->i_uid = attr->ia_uid;

  if ( FlagOn( ia_valid, ATTR_GID ) )
    i->i_gid = attr->ia_gid;

  if ( FlagOn( ia_valid, ATTR_ATIME ) ) {
#if defined UFSD_FAT
    if ( is_fat( &sbi->options ) ) {
      // fat access time - one day
      int bias    = 60 * (-1 == sbi->options.bias? sys_tz.tz_minuteswest : sbi->options.bias);
      unsigned ad = (attr->ia_atime.tv_sec - bias) / (24*60*60);
      unsigned id = (i->i_atime.tv_sec - bias) / (24*60*60);
      if ( ad != id ) {
        i->i_atime.tv_sec   = ad * (24*60*60) + bias;
        i->i_atime.tv_nsec  = 0;
      } else {
        ClearFlag( ia_valid, ATTR_ATIME );
      }
    } else
#endif
    if ( !ufsd_time_trunc( sbi, &attr->ia_atime, &i->i_atime ) ) {
      ClearFlag( ia_valid, ATTR_ATIME );
    }
  }

  if ( FlagOn( ia_valid, ATTR_MTIME ) && !ufsd_time_trunc( sbi, &attr->ia_mtime, &i->i_mtime ) ) {
    ClearFlag( ia_valid, ATTR_MTIME );
  }

  if ( FlagOn( ia_valid, ATTR_CTIME ) && !ufsd_time_trunc( sbi, &attr->ia_ctime, &i->i_ctime ) ) {
    ClearFlag( ia_valid, ATTR_CTIME );
  }

  if ( FlagOn( ia_valid, ATTR_MODE ) ) {
    umode_t mode = attr->ia_mode;
//    DebugTrace( 0, Dbg, ("mode %u -> %u", i->i_mode, mode ));
    if ( !in_group_p( i->i_gid ) && !capable( CAP_FSETID ) )
      mode &= ~S_ISGID;

    if ( mode == i->i_mode )
      ClearFlag( ia_valid, ATTR_MODE );
    else {
      i->i_mode = mode;
#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
      if ( is_ntfs( &sbi->options ) || is_hfs( &sbi->options ) ) {
        err = ufsd_acl_chmod( i );
        if ( err )
          goto out;
      }
#endif
    }
  }

out:
  if ( ia_valid & (ATTR_SIZE | ATTR_UID | ATTR_GID | ATTR_ATIME | ATTR_MTIME | ATTR_CTIME | ATTR_MODE ) ) {
    if ( unlikely( !is_bdi_ok( i->i_sb ) ) )
      err = -ENODEV;
    else
      mark_inode_dirty_sync( i );
  }

  VfsTrace( -1, Dbg, ("setattr -> %d, uid=%d,gid=%d,m=%o,sz=%llx,%llx%s", err,
                        __kuid_val(i->i_uid), __kgid_val(i->i_gid), i->i_mode,
                        u->valid, i->i_size, FlagOn(i->i_state, I_DIRTY)?",d":"" ));

  CheckTime( 2 );
  ufsd_check_sp();

  return err;
}


#ifdef UFSD_NTFS

#if is_decl( INODE_GETATTR_V1 )

  #define GETATTR_DECLARE_ARG IN struct vfsmount  *mnt,                 \
                              IN struct dentry    *de,                  \
                              OUT struct kstat    *kstat
  #define GETATTR_DENTRY_VAR

#elif is_decl( INODE_GETATTR_V2 )

  #define GETATTR_DECLARE_ARG IN const struct path *path,               \
                              OUT struct kstat     *kstat,              \
                              IN u32                request_mask,       \
                              IN unsigned int       flags
  #define GETATTR_DENTRY_VAR  struct dentry *de = path->dentry;

#endif

///////////////////////////////////////////////////////////
// ufsd_getattr
//
// inode_operations::getattr
///////////////////////////////////////////////////////////
static int
ufsd_getattr(
    GETATTR_DECLARE_ARG
    )
{
  GETATTR_DENTRY_VAR
  struct inode *i = de->d_inode;
  unode *u = UFSD_U( i );

  generic_fillattr( i, kstat );

  if ( is_sparsed_or_compressed( u ) )
    kstat->blocks = u->total_alloc >> 9;

#if 0
  DebugTrace( 0, Dbg, ("getattr (r=%llx%s): m=%o, t=%lx+%lu, %lx+%lu, %lx+%lu, s=%llx, b=%llx",
                      kstat->ino, is_fork( u )? ",fork":"", (unsigned)kstat->mode,
                      kstat->atime.tv_sec, kstat->atime.tv_nsec,
                      kstat->mtime.tv_sec, kstat->mtime.tv_nsec,
                      kstat->ctime.tv_sec, kstat->ctime.tv_nsec,
                      kstat->size, kstat->blocks ));
#endif

#if 0
  DebugTrace( 0, Dbg, ("getattr (r=%llx): m=%o, s=%llx, b=%llx",
                      kstat->ino, (unsigned)kstat->mode,
                      kstat->size, kstat->blocks ));
#endif

  return 0;
}
#else
#define ufsd_getattr NULL
#endif


///////////////////////////////////////////////////////////
// ufsd_rename
//
// inode_operations::rename
///////////////////////////////////////////////////////////
static int
ufsd_rename(
    IN struct inode   *odir,
    IN struct dentry  *ode,
    IN struct inode   *ndir,
    IN struct dentry  *nde
#if is_decl( INODE_RENAME_V2 )
    , IN unsigned int flags
#endif
    )
{
  int err;
  usuper *sbi = UFSD_SB( odir->i_sb );
  UINT64 odir_size = 0, ndir_size = 0;

  VfsTrace( +1, Dbg, ("rename: r=%lx, %p('%.*s') => r=%lx, %p('%.*s')",
                      odir->i_ino, ode,
                      (int)ode->d_name.len, ode->d_name.name,
                      ndir->i_ino, nde,
                      (int)nde->d_name.len, nde->d_name.name ));

#if is_decl( INODE_RENAME_V2 )
  if (flags) {
    err = -EINVAL;
    goto out;
  }
#endif

  //
  // If the target already exists, delete it first.
  // I will not unwind it on move failure. Although it's a weak point
  // it's better to not have it implemented then trying to create
  // a complex workaround.
  //
  if ( NULL != nde->d_inode ) {

    DebugTrace( 0, Dbg, ("rename: deleting existing target %p (r=%lx)", nde->d_inode, nde->d_inode->i_ino));

    dget( nde );
    err = ufsd_unlink( ndir, nde );
    dput( nde );
    if ( unlikely( 0 != err ) )
      goto out;
  }

  lock_ufsd( sbi );

  err = ufsd_open_by_id( sbi, odir );
  if ( likely( 0 == err ) ) {
    err = ufsd_open_by_id( sbi, ndir );
    if ( likely( 0 == err ) ) {
      err = ufsd_open_by_id( sbi, ode->d_inode );
      if ( likely( 0 == err ) ) {
        err = ufsdapi_file_move( sbi->ufsd, UFSD_FH( odir ), UFSD_FH( ndir ), UFSD_FH( ode->d_inode ),
                                 ode->d_name.name, ode->d_name.len,
                                 nde->d_name.name, nde->d_name.len,
                                 &odir_size, &ndir_size );
      }
    }
  }

  unlock_ufsd( sbi );

  if ( unlikely( 0 != err ) )
    goto out;

  // Mark dir as requiring resync.
  odir->i_ctime = odir->i_mtime = ufsd_inode_current_time( sbi );
  mark_inode_dirty( odir );
  mark_inode_dirty( ndir );
  i_size_write( odir, odir_size );
  inode_set_bytes( odir, odir_size );

  if ( ndir != odir ) {
    ndir->i_mtime  = ndir->i_ctime = odir->i_ctime;
    i_size_write( ndir, ndir_size );
    inode_set_bytes( ndir, ndir_size );

#ifdef UFSD_COUNT_CONTAINED
    if ( S_ISDIR( ode->d_inode->i_mode ) ) {
      assert(odir->i_nlink > 0);
      drop_nlink( odir );
      inc_nlink( ndir );
    }
#endif
  }

  if ( NULL != ode->d_inode ) {
    ode->d_inode->i_ctime = odir->i_ctime;
    mark_inode_dirty( ode->d_inode );
  }

out:
  VfsTrace( -1, Dbg, ("rename -> %d", err));
  return err;
}


#if defined UFSD_NTFS || defined UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_getxattr_hlp
//
// Helper function
///////////////////////////////////////////////////////////
noinline static int
ufsd_getxattr_hlp(
    IN  struct inode  *i,
    IN  const char    *name,
    OUT void          *value,
    IN  size_t        size,
    OUT size_t        *required
    )
{
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  int ret;
  size_t len;

  if ( NULL != u->ufile && !is_xattr( u ) )
    return -ENODATA;

  if ( unlikely( is_fork( u ) ) ) {
    ret = -EOPNOTSUPP;
    goto out;
  }

  if ( NULL == required )
    lock_ufsd( sbi );

  ret = ufsd_open_by_id( sbi, i );
  if ( likely( 0 == ret ) )
    ret = ufsdapi_get_xattr( sbi->ufsd, u->ufile, name, strlen(name), value, size, &len );
  if ( 0 == ret ) {
    ret = (int)len;
    assert( is_xattr( u ) );
  } else {
    // ERR_NOFILEEXISTS -> -ENOENT -> -ENODATA
    if ( -ENOENT == ret )
      ret = -ENODATA;
    else if ( -ERANGE == ret && NULL != required )
      *required = len;
  }

  if ( NULL == required )
    unlock_ufsd( sbi );

out:
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_setxattr_hlp
//
// Helper function
///////////////////////////////////////////////////////////
noinline static int
ufsd_setxattr_hlp(
    IN struct inode *i,
    IN const char   *name,
    IN const void   *value,
    IN size_t       size,
    IN int          flags,
    IN int          locked
    )
{
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  int ret;
  C_ASSERT( 1 == XATTR_CREATE && 2 == XATTR_REPLACE );

  if ( unlikely( is_fork( u ) ) ) {
    ret = -EOPNOTSUPP;
    goto out;
  }

  if ( !locked )
    lock_ufsd( sbi );

  ret = ufsd_open_by_id( sbi, i );
  if ( likely( 0 == ret ) )
    ret = ufsdapi_set_xattr( sbi->ufsd, u->ufile, name, strlen(name), value, size, flags );
  if ( 0 == ret ) {
    // Check if we delete the last xattr ( 0 == size && XATTR_REPLACE == flags && no xattrs )
    if ( 0 != size
      || XATTR_REPLACE != flags
      || 0 != ufsdapi_list_xattr( sbi->ufsd, u->ufile, NULL, 0, &size )
      || 0 != size ) {
      set_bit( UFSD_UNODE_FLAG_EA_BIT, &u->flags );
    } else {
      clear_bit( UFSD_UNODE_FLAG_EA_BIT, &u->flags );
      DebugTrace( 0, UFSD_LEVEL_XATTR, ("setxattr: (removed last extended attribute)" ));
    }
    // Ok, ret is already 0
  } else {
    // ERR_NOFILEEXISTS -> -ENOENT -> -ENODATA
    if ( -ENOENT == ret )
      ret = -ENODATA;
  }

  if ( !locked )
    unlock_ufsd( sbi );

out:
  return ret;
}

#ifdef CONFIG_FS_POSIX_ACL

#if is_decl( POSIX_ACL_TO_XATTR_V2 )
#if defined HAVE_LINUX_PROC_NS_H && HAVE_LINUX_PROC_NS_H
  #include <linux/proc_ns.h>
#endif
#include <linux/user_namespace.h>
  // wait for 'init_user_ns' to be non G.P.L.
  struct user_namespace user_ns = {
    .uid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .gid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .projid_map = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
#if is_struct( USER_NAMESPACE_COUNT )
    .count = ATOMIC_INIT(3),
#else
    .kref = { .refcount = ATOMIC_INIT(3), },
#endif
    .owner = GLOBAL_ROOT_UID,
    .group = GLOBAL_ROOT_GID,
#if is_struct( USER_NAMESPACE_PROC_INUM )
    .proc_inum = PROC_USER_INIT_INO,
#endif
  };
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( &user_ns, acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( &user_ns, value, size )
#else
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( value, size )
#endif


#if !defined POSIX_ACL_XATTR_ACCESS && defined XATTR_NAME_POSIX_ACL_ACCESS
#define POSIX_ACL_XATTR_ACCESS XATTR_NAME_POSIX_ACL_ACCESS
#endif

#if !defined POSIX_ACL_XATTR_DEFAULT && defined XATTR_NAME_POSIX_ACL_DEFAULT
#define POSIX_ACL_XATTR_DEFAULT XATTR_NAME_POSIX_ACL_DEFAULT
#endif

#define POSIX_ACL_XATTR_ACCESS_LEN          ( sizeof( POSIX_ACL_XATTR_ACCESS ) - 1 )
#define POSIX_ACL_XATTR_DEFAULT_LEN         ( sizeof( POSIX_ACL_XATTR_DEFAULT ) - 1 )


#if ( ! ( is_decl( KFREE_CALL_RCU ) ) ) \
 && ( ! ( is_decl( RCU_IS_WATCHING ) ) ) \
 && ( ! ( is_decl( GET_CACHED_ACL_RCU ) ) )

// 2.6.30..3.0: {set,get}_cached_acl() calls are accessible
#define POSIX_ACL_API_KERNEL 1

#elif ( ! ( is_decl( KFREE_CALL_RCU ) ) ) \
 && ( ! ( is_decl( RCU_IS_WATCHING ) ) ) \
 && ( is_decl( GET_CACHED_ACL_RCU ) )

// 3.1..3.13: {set,get}_cached_acl() calls are restricted
#define POSIX_ACL_API_UFSD 1

#elif ( is_decl( RCU_IS_WATCHING ) ) \
 || ( ! ( is_decl( KFREE_CALL_RCU ) ) )

#if is_decl( __POSIX_ACL_CHMOD )
// 3.14..4.7+: {set,get}_cached_acl() calls are accessible again
#define POSIX_ACL_API_KERNEL 1
#else
// fallback branch for cases between 3.13 and 3.14
#define POSIX_ACL_API_UFSD 1
#endif

#else

// fallback branch just in case
#define POSIX_ACL_API_UFSD 1

#endif

// tiny helper
#define Is_cached_acl(__acl) (ACL_NOT_CACHED != __acl)

#ifdef POSIX_ACL_API_KERNEL
#define Set_cached_acl( _inode, _type, _acl, _ptr )  set_cached_acl( _inode, _type, _acl )
#else // use own stub in case of missing
#define Set_cached_acl( _inode, _type, _acl, _ptr )  ufsd_set_cached_acl( _inode, _type, _acl, _ptr )
#endif


///////////////////////////////////////////////////////////
// ufsd_posix_acl_release
//
//
///////////////////////////////////////////////////////////
static inline void
ufsd_posix_acl_release(
    IN struct posix_acl *acl
    )
{
  if ( NULL == acl )
    return;
  if ( atomic_dec_and_test( &acl->a_refcount ) )
    kfree( acl );
}


///////////////////////////////////////////////////////////
// ufsd_set_cached_acl
//
//
///////////////////////////////////////////////////////////
static inline void ufsd_set_cached_acl(
    IN struct inode      *i,
    IN int                type,
    IN struct posix_acl  *acl,
    IN struct posix_acl **p
    )
{
  struct posix_acl *old;
  if ( NULL == p )
    p = ACL_TYPE_ACCESS == type? &i->i_acl : &i->i_default_acl;
  spin_lock( &i->i_lock );
  old = *p;
  *p  = NULL == acl? ACL_NOT_CACHED : posix_acl_dup( acl );
  spin_unlock( &i->i_lock );
  if ( Is_cached_acl( old ) )
    ufsd_posix_acl_release( old );
  return;
}


///////////////////////////////////////////////////////////
// ufsd_get_acl_ex
//
// Helper function for ufsd_get_acl
///////////////////////////////////////////////////////////
static struct posix_acl*
ufsd_get_acl_ex(
    IN struct inode *i,
    IN int          type,
    IN int          locked
    )
{
  const char *name;
  struct posix_acl *acl;
  size_t req;
  int ret;
  usuper *sbi = UFSD_SB( i->i_sb );
  DEBUG_ONLY( const char *hint = ""; )

#ifdef POSIX_ACL_API_UFSD
  unode *u    = UFSD_U( i );
  struct posix_acl **p;
#endif

  DebugTrace( +1, Dbg, ("ufsd_get_acl r=%lx, %d, %d", i->i_ino, type, locked ));

  assert( sbi->options.acl );

#ifdef POSIX_ACL_API_UFSD

  switch ( type ) {
  case ACL_TYPE_ACCESS:   p = &i->i_acl; break;
  case ACL_TYPE_DEFAULT:  p = &i->i_default_acl; break;
  default:
    acl = ERR_PTR(-EINVAL);
    DEBUG_ONLY( hint = "unknown type"; )
    goto out;
  }

  //
  // Check cached value of 'acl' and 'default_acl'
  //
  spin_lock( &i->i_lock );
  acl = *p;
  if ( Is_cached_acl( acl ) )
    acl = posix_acl_dup( acl );
  else if ( NULL != u->ufile && !is_xattr(u) )
    acl = NULL;
  spin_unlock( &i->i_lock );

  if ( Is_cached_acl( acl ) ) {
    DEBUG_ONLY( hint = "cached"; )
    goto out;
  }

#endif

  //
  // Possible values of 'type' was already checked above
  //
  name = ACL_TYPE_ACCESS == type? POSIX_ACL_XATTR_ACCESS : POSIX_ACL_XATTR_DEFAULT;

  if ( !locked )
    lock_ufsd( sbi );

  //
  // Get the size of extended attribute
  //
  req = 0; // make clang happy
  ret = ufsd_getxattr_hlp( i, name, sbi->x_buffer, sbi->bytes_per_xbuffer, &req );

  if ( (ret > 0 && NULL == sbi->x_buffer) || -ERANGE == ret ) {

    //
    // Allocate/Reallocate buffer and read again
    //
    if ( NULL != sbi->x_buffer ) {
      assert( -ERANGE == ret );
      kfree( sbi->x_buffer );
    }

    if ( ret > 0 )
      req = ret;

    sbi->x_buffer = kmalloc( req, GFP_NOFS );
    if ( NULL != sbi->x_buffer ) {
      sbi->bytes_per_xbuffer = req;

      //
      // Read the extended attribute.
      //
      ret = ufsd_getxattr_hlp( i, name, sbi->x_buffer, sbi->bytes_per_xbuffer, &req );
      assert( ret > 0 );

    } else {
      ret = -ENOMEM;
      sbi->bytes_per_xbuffer = 0;
    }
  }

  if ( !locked )
    unlock_ufsd( sbi );

  //
  // Translate extended attribute to acl
  //
  if ( ret > 0 ) {
    acl = Posix_acl_from_xattr( sbi->x_buffer, ret );
    if ( !IS_ERR( acl ) )
      Set_cached_acl( i, type, acl, p );
  } else {
    acl = -ENODATA == ret || -ENOSYS == ret ? NULL : ERR_PTR( ret );
  }

  DEBUG_ONLY( hint = "ufsd"; )

#ifdef POSIX_ACL_API_UFSD
out:
#endif
  DebugTrace( -1, Dbg, ("ufsd_get_acl -> %p, %s", acl, hint ));
  return acl;
}


///////////////////////////////////////////////////////////
// ufsd_get_acl
//
// inode_operations::get_acl
// inode lock (inode->i_mutex / inode->i_rwsem): don't care
///////////////////////////////////////////////////////////
static struct posix_acl*
ufsd_get_acl(
    IN struct inode *i,
    IN int          type
    )
{
  return ufsd_get_acl_ex( i, type, 0 );
}


///////////////////////////////////////////////////////////
// ufsd_set_acl_ex
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_set_acl_ex(
    IN struct inode     *i,
    IN struct posix_acl *acl,
    IN int              type,
    IN int              locked
    )
{
  const char *name;
  void *value = NULL;
  size_t size = 0;
  int err     = 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  assert( UFSD_SB( i->i_sb )->options.acl );

  switch( type ) {
    case ACL_TYPE_ACCESS:
      if ( NULL != acl ) {
        posix_acl_mode mode = i->i_mode;
        err = posix_acl_equiv_mode( acl, &mode );
        if ( err < 0 )
          return err;

        if ( i->i_mode != mode ) {
          i->i_mode = mode;
          mark_inode_dirty( i );
        }
        if ( 0 == err )
          acl = NULL; // acl can be exactly represented in the traditional file mode permission bits
      }
      name = POSIX_ACL_XATTR_ACCESS;
      break;

    case ACL_TYPE_DEFAULT:
      if ( !S_ISDIR( i->i_mode ) )
        return acl ? -EACCES : 0;
      name = POSIX_ACL_XATTR_DEFAULT;
      break;

    default:
      return -EINVAL;
  }

  if ( NULL != acl ) {
    size  = posix_acl_xattr_size( acl->a_count );
    value = kmalloc( size, GFP_NOFS );
    if ( NULL == value )
      return -ENOMEM;

    err = Posix_acl_to_xattr( acl, value, size );
    if ( err < 0 )
      return err;
  }

  if ( 0 == ( err = ufsd_setxattr_hlp( i, name, value, size, 0, locked ) ) )
    Set_cached_acl( i, type, acl, NULL );

  kfree( value );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_set_acl
//
// inode_operations::set_acl
///////////////////////////////////////////////////////////
static int
ufsd_set_acl(
    IN struct inode     *i,
    IN struct posix_acl *acl,
    IN int              type
    )
{
  // last 0 means: ufsd not locked yet
  return ufsd_set_acl_ex( i, acl, type, 0 );
}


//
// To access to hfs fork "resource" we should allow to 'exec' (not open) generic hfs file
//
#ifdef UFSD_USE_HFS_FORK
#ifdef UFSD_HFS_ONLY
#define FIX_HFS_FORK_PERMISSON( err, i, mask )                \
  if ( -EACCES == err                                         \
    && S_ISREG( (i)->i_mode )                                 \
    && ( MAY_EXEC == ( mask & (MAY_EXEC|MAY_OPEN) ) )         \
    && !is_fork( UFSD_U( (i) ) ) ) {                          \
    DebugTrace( 0, Dbg, ("fix permission to allow fork access (r=%lx, %o, mask=%x))", (i)->i_ino, (i)->i_mode, mask) ); \
    err = 0;  \
  }
#elif defined UFSD_HFS
#define FIX_HFS_FORK_PERMISSON( err, i, mask )                \
  if ( -EACCES == err                                         \
    && S_ISREG( (i)->i_mode )                                 \
    && ( MAY_EXEC == ( mask & (MAY_EXEC|MAY_OPEN) ) )         \
    && UFSD_SB( (i)->i_sb )->options.hfs                      \
    && !is_fork( UFSD_U( (i) ) ) ) {                          \
    DebugTrace( 0, Dbg, ("fix permission to allow fork access (r=%lx, %o, mask=%x))", (i)->i_ino, (i)->i_mode, mask) ); \
    err = 0;  \
  }
#endif
#endif

#ifndef FIX_HFS_FORK_PERMISSON
  #define FIX_HFS_FORK_PERMISSON( err, i, mask )
#endif

#if is_decl( GENERIC_PERMISSION_V3 )

#ifdef UFSD_USE_HFS_FORK
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask
    )
{
  usuper *sbi   = UFSD_SB( i->i_sb );
  int err = 0;
  if ( sbi->options.no_acs_rules ) {
    // "no access rules" mode - allow all changes
    return err;
  }
  //
  // Call default function
  //
  err = generic_permission( i, mask );
  FIX_HFS_FORK_PERMISSON( err, i, mask )
  return err;
}
#else
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask
    )
{
  usuper *sbi   = UFSD_SB( i->i_sb );
  int err = 0;
  if ( sbi->options.no_acs_rules ) {
    // "no access rules" mode - allow all changes
    return err;
  }
  //
  // Call default function
  //
  err = generic_permission( i, mask );
  return err;
}
#endif

#else

///////////////////////////////////////////////////////////
// ufsd_check_acl
//
// Helper function for ufsd_permission
///////////////////////////////////////////////////////////
static int
ufsd_check_acl(
    IN struct inode   *i,
    IN int            mask
#ifdef IPERM_FLAG_RCU
    , IN unsigned int flags
#endif
    )
{
  int err;
  struct posix_acl *acl;

  assert( UFSD_SB( i->i_sb )->options.acl );

#ifdef IPERM_FLAG_RCU
  if ( flags & IPERM_FLAG_RCU ) {
    if ( !negative_cached_acl( i, ACL_TYPE_ACCESS ) )
      return -ECHILD;
    return -EAGAIN;
  }
#endif

  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR( acl ) )
    return PTR_ERR( acl );

  if ( NULL == acl )
    return -EAGAIN;

  //
  // Trace acl
  //
#if 0//def UFSD_DEBUG
  {
    int n;
    for ( n = 0; n < acl->a_count; n++ ) {
      DebugTrace( 0, Dbg, ("e_tag=%x, e_perm=%x e_id=%x", (unsigned)acl->a_entries[n].e_tag, (unsigned)acl->a_entries[n].e_perm, (unsigned)acl->a_entries[n].e_id ));
    }
  }
#endif

  err = posix_acl_permission( i, acl, mask );
  ufsd_posix_acl_release( acl );

  DebugTrace( 0, Dbg, ("check_acl (r=%lx, m=%o) -> %d", i->i_ino, mask, err) );

  return err;
}


#ifdef IPERM_FLAG_RCU
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask,
    IN unsigned int flag
    )
{
  usuper *sbi   = UFSD_SB( i->i_sb );
  int err = 0;
  if ( sbi->options.no_acs_rules ) {
    // "no access rules" mode - allow all changes
    return err;
  }
  err = generic_permission( i, mask, flag, ufsd_check_acl );
  FIX_HFS_FORK_PERMISSON( err, i, mask )
  return err;
}
#else
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask
#if is_decl( INOP_PERMISSION_V1 )
    , IN struct nameidata *nd
#endif
#if is_decl( INOP_PERMISSION_V2 )
    , IN unsigned int ui
#endif
    )
{
  usuper *sbi   = UFSD_SB( i->i_sb );
  int err = 0;
  if ( sbi->options.no_acs_rules ) {
    // "no access rules" mode - allow all changes
    return err;
  }
  err = generic_permission( i, mask, ufsd_check_acl );
  FIX_HFS_FORK_PERMISSON( err, i, mask )
  return err;
}
#endif // #ifdef IPERM_FLAG_RCU
#endif // #if is_decl( GENERIC_PERMISSION_V3 )


///////////////////////////////////////////////////////////
// ufsd_acl_chmod
//
//
///////////////////////////////////////////////////////////
static int
ufsd_acl_chmod(
    IN struct inode *i
    )
{
#if !( is_decl( POSIX_ACL_CHMOD_V2 ) )
  struct posix_acl *acl;
#endif
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  DebugTrace( +1, Dbg, ("acl_chmod r=%lx", i->i_ino));
#if is_decl( POSIX_ACL_CHMOD_V2 )
  err = posix_acl_chmod( i, i->i_mode );
#else
  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR( acl ) || !acl )
    err = PTR_ERR( acl );
  else {
#if is_decl( POSIX_ACL_CHMOD_V1 )
    err = posix_acl_chmod( &acl, GFP_NOFS, i->i_mode );
    if ( err ) {
      DebugTrace( -1, Dbg, ("acl_chmod -> %d", err));
      return err;
    }
    err = ufsd_set_acl( i, acl, ACL_TYPE_ACCESS );
    ufsd_posix_acl_release( acl );
#else
    struct posix_acl *clone = posix_acl_clone( acl, GFP_NOFS );
    ufsd_posix_acl_release( acl );
    if ( NULL == clone )
      err = -ENOMEM;
    else {
      err = posix_acl_chmod_masq( clone, i->i_mode );
      if ( 0 == err )
        err = ufsd_set_acl( i, clone, ACL_TYPE_ACCESS );
      ufsd_posix_acl_release( clone );
    }
#endif
  }
#endif

  DebugTrace( -1, Dbg, ("acl_chmod -> %d", err));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_get_acl
//
// Helper function for ufsd_xattr_acl_access_get/ufsd_xattr_acl_default_get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_get_acl(
    IN struct inode *i,
    IN int          type,
    OUT void        *buffer,
    IN size_t       size
    )
{
  struct posix_acl *acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  acl = ufsd_get_acl( i, type );
  if ( IS_ERR( acl ) )
    return PTR_ERR( acl );

  if ( NULL == acl )
    return -ENODATA;

  err = Posix_acl_to_xattr( acl, buffer, size );
  ufsd_posix_acl_release( acl );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_set_acl
//
// Helper function for ufsd_xattr_acl_access_set/ufsd_xattr_acl_default_set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_set_acl(
    IN struct inode *i,
    IN int          type,
    IN const void   *value,
    IN size_t       size
    )
{
  struct posix_acl *acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  if ( !inode_owner_or_capable( i ) )
    return -EPERM;

  if ( NULL == value )
    acl = NULL;
  else {
    acl = Posix_acl_from_xattr( value, size );
    if ( IS_ERR( acl ) )
      return PTR_ERR(acl);

    if ( NULL != acl ) {
#if is_decl( POSIX_ACL_VALID_V2 )
      err = posix_acl_valid( i->i_sb->s_user_ns, acl );
#elif is_decl( POSIX_ACL_VALID_V1 )
      err = posix_acl_valid( acl );
#endif
      if ( err )
        goto release_and_out;
    }
  }

  err = ufsd_set_acl( i, acl, type );

release_and_out:
  ufsd_posix_acl_release( acl );
  return err;
}
#else
  #define ufsd_permission NULL
  #define ufsd_get_acl    NULL
  #define ufsd_set_acl    NULL
#endif // #ifdef CONFIG_FS_POSIX_ACL
#else
  #define ufsd_setxattr_hlp( ... ) -EOPNOTSUPP
  #define ufsd_getxattr_hlp( ... ) -EOPNOTSUPP
#endif // #if defined UFSD_NTFS || defined UFSD_HFS

#define UFSD_XATTR_SYSTEM_DOS_ATTRIB         "system.dos_attrib"
#define UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN     ( sizeof( UFSD_XATTR_SYSTEM_DOS_ATTRIB ) - 1 )
#define UFSD_XATTR_SYSTEM_NTFS_ATTRIB        "system.ntfs_attrib"
#define UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN    ( sizeof( UFSD_XATTR_SYSTEM_NTFS_ATTRIB ) - 1 )
#define UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE     "system.ntfs_attrib_be"
#define UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN ( sizeof( UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE ) - 1 )

#define SAMBA_PROCESS_NAME                   "smbd"
#define SAMBA_PROCESS_NAME_LEN               ( sizeof( SAMBA_PROCESS_NAME ) - 1 )
#define UFSD_XATTR_USER_DOSATTRIB            "user.DOSATTRIB"
#define UFSD_XATTR_USER_DOSATTRIB_LEN        ( sizeof( UFSD_XATTR_USER_DOSATTRIB ) - 1 )


///////////////////////////////////////////////////////////
// ufsd_getxattr
//
// inode_operations::getxattr
///////////////////////////////////////////////////////////
#if is_decl( INODE_OPS_XATTR_ANY )
static ssize_t
#else
static int
#endif
ufsd_getxattr(
#if !is_decl( INODE_OPS_XATTR_ANY )
    IN const struct xattr_handler *handler,
#endif
    IN struct dentry  *de,
#if is_decl( INODE_OPS_XATTR_V2 ) || !is_decl( INODE_OPS_XATTR_ANY )
    IN struct inode   *i,
#endif
    IN const char     *name,
    OUT void          *buffer,
    IN size_t         size
    )
{
#if is_decl( INODE_OPS_XATTR_ANY )
  ssize_t err;
#else
  int err;
#endif
#if is_decl( INODE_OPS_XATTR_V1 )
  struct inode *i = de->d_inode;
#endif
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  size_t name_len = strlen( name );
  unsigned int attr_len;

  VfsTrace( +1, UFSD_LEVEL_XATTR, ("getxattr: r=%lx, \"%s\", %zu", i->i_ino, name, size ));

  if ( unlikely( is_fork( u ) ) ) {
    err = -EOPNOTSUPP;
    goto out;
  }

  //
  // Dispatch request
  //
  if ( ( UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN == name_len
        && ( attr_len  = sizeof(unsigned char), 0 == memcmp( name, UFSD_XATTR_SYSTEM_DOS_ATTRIB, UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN + 1 ) ) )
      || ( UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN == name_len
        && ( attr_len  = sizeof(unsigned int), 0 == memcmp( name, UFSD_XATTR_SYSTEM_NTFS_ATTRIB, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN + 1 ) ) )
      || ( UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN == name_len
        && ( attr_len  = sizeof(unsigned int), 0 == memcmp( name, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN + 1 ) ) )
      || ( UFSD_XATTR_USER_DOSATTRIB_LEN == name_len
        && ( attr_len  = 5, !is_hfs( &sbi->options ) )
        && 0 == memcmp( current->comm, SAMBA_PROCESS_NAME, SAMBA_PROCESS_NAME_LEN + 1 )
        && 0 == memcmp( name, UFSD_XATTR_USER_DOSATTRIB, UFSD_XATTR_USER_DOSATTRIB_LEN + 1 ) ) ) {

    // dos_attrib
    if ( NULL == buffer )
      err = attr_len;
    else if ( size < attr_len )
      err = -ENODATA;
    else {
      unsigned int attrib;

      lock_ufsd( sbi );
      err = ufsd_open_by_id( sbi, i );
      if ( likely( 0 == err ) )
        err = ufsdapi_get_dosattr( sbi->ufsd, u->ufile, &attrib );
      unlock_ufsd( sbi );

      if ( 0 == err ) {
        err = attr_len;
        switch( name_len ) {
          case UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN:
            *(unsigned char*)buffer = attrib;
            break;
          case UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN:
            *(unsigned int*)buffer = attrib;
            break;
          case UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN:
            *(unsigned int*)buffer = cpu_to_be32( attrib );
            break;
          case UFSD_XATTR_USER_DOSATTRIB_LEN:
            // sprintf returns the total number of characters written, last zero is not included
            err = sprintf( (char*)buffer, "0x%x", attrib & 0xff ) + 1;
            assert( err <= 5 );
            DebugTrace( 0, Dbg, ("user.DOSATTRIB=%s", (char*)buffer ));
        }
      }
    }
  }
#if defined CONFIG_FS_POSIX_ACL && ( defined UFSD_NTFS || defined UFSD_HFS )
  else if ( ( POSIX_ACL_XATTR_ACCESS_LEN == name_len
        && 0 == memcmp( name, POSIX_ACL_XATTR_ACCESS, POSIX_ACL_XATTR_ACCESS_LEN + 1 ) )
      || ( POSIX_ACL_XATTR_DEFAULT_LEN == name_len
        && 0 == memcmp( name, POSIX_ACL_XATTR_DEFAULT, POSIX_ACL_XATTR_DEFAULT_LEN + 1 ) ) ) {
    err = sbi->options.acl
      ? ufsd_xattr_get_acl( i, POSIX_ACL_XATTR_ACCESS_LEN == name_len? ACL_TYPE_ACCESS : ACL_TYPE_DEFAULT, buffer, size )
      : -EOPNOTSUPP;
  }
#endif
  else {
    err = ufsd_getxattr_hlp( i, name, buffer, size, NULL );
  }
out:
  VfsTrace( -1, UFSD_LEVEL_XATTR, ("getxattr -> %d", (int)err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_setxattr
//
// inode_operations::setxattr
///////////////////////////////////////////////////////////
noinline static int
ufsd_setxattr(
#if !is_decl( INODE_OPS_XATTR_ANY )
    IN const struct xattr_handler *handler,
#endif
    IN struct dentry  *de,
#if is_decl( INODE_OPS_XATTR_V2 ) || !is_decl( INODE_OPS_XATTR_ANY )
    IN struct inode   *i,
#endif
    IN const char     *name,
    IN const void     *value,
    IN size_t         size,
    IN int            flags
    )
{
  int err;
#if is_decl( INODE_OPS_XATTR_V1 )
  struct inode *i = de->d_inode;
#endif
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  size_t name_len = strlen( name );
  TRACE_ONLY( const char* hint = NULL == value && 0 == size && XATTR_REPLACE == flags? "removexattr" : "setxattr" ); // add hint for "replace"

  VfsTrace( +1, UFSD_LEVEL_XATTR, ("%s: r=%lx, \"%s\", %zu, %d", hint, i->i_ino, name, size, flags ));

  if ( unlikely( is_fork( u ) ) ) {
    err = -EOPNOTSUPP;
    goto out;
  }

  //
  // Dispatch request
  //
  if ( ( UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN == name_len
        && 0 == memcmp( name, UFSD_XATTR_SYSTEM_DOS_ATTRIB, UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN + 1 ) )
      || ( UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN == name_len
        && 0 == memcmp( name, UFSD_XATTR_SYSTEM_NTFS_ATTRIB, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN + 1 ) )
      || ( UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN == name_len
        && 0 == memcmp( name, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE, UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN + 1 ) )
      || ( UFSD_XATTR_USER_DOSATTRIB_LEN == name_len
        && !is_hfs( &sbi->options )
        && 0 == memcmp( current->comm, SAMBA_PROCESS_NAME, SAMBA_PROCESS_NAME_LEN + 1 )
        && 0 == memcmp( name, UFSD_XATTR_USER_DOSATTRIB, UFSD_XATTR_USER_DOSATTRIB_LEN + 1 ) ) ) {

    // dos_attrib
    unsigned int attrib = 0; // not necessary just to suppress warnings
    err = -EINVAL;
    if ( NULL == value )
      goto out;

    switch( name_len ) {
    case UFSD_XATTR_SYSTEM_DOS_ATTRIB_LEN:
      if ( sizeof(unsigned char) != size )
        goto out;
      attrib = *(unsigned char*)value;
      break;
    case UFSD_XATTR_SYSTEM_NTFS_ATTRIB_LEN:
      if ( sizeof(unsigned int) != size )
        goto out;
      attrib = *(unsigned int*)value;
      break;
    case UFSD_XATTR_SYSTEM_NTFS_ATTRIB_BE_LEN:
      if ( sizeof(unsigned int) != size )
        goto out;
      attrib = be32_to_cpu( *(unsigned int*)value );
      break;
    case UFSD_XATTR_USER_DOSATTRIB_LEN:
      if ( size < 4 || 0 != ((char*)value)[size-1] )
        goto out;
      // The input value must be string in form 0x%x with last zero
      // This means that the 'size' must be 4, 5, ...
      // E.g: 0x1 - 4 bytes, 0x20 - 5 bytes
      if ( 1 != sscanf( (char*)value, "0x%x", &attrib ) )
        goto out;
      if ( i->i_size <= 1 && ( attrib & UFSDAPI_SYSTEM ) ) {
        // Do not allow to set 'System' attribute if file size is <= 1.
        // If file size == 0 && 'System' attribute is set, then such
        // a file is considered to be FIFO.
        // If file size == 1 && 'System' attribute is set, then such
        // a file is considered to be socket.
        VfsTrace( 0, UFSD_LEVEL_ERROR, ("user.DOSATTRIB: Cannot set 'System' attribute on file with size %llu", i->i_size ));
        goto out;
      }
    }

    lock_ufsd( sbi );
    err = ufsd_open_by_id( sbi, i );
    if ( likely( 0 == err ) )
      err = ufsdapi_set_dosattr( sbi->ufsd, u->ufile, attrib );
    unlock_ufsd( sbi );
  }
#if defined CONFIG_FS_POSIX_ACL && ( defined UFSD_NTFS || defined UFSD_HFS )
  else if ( ( POSIX_ACL_XATTR_ACCESS_LEN == name_len
        && 0 == memcmp( name, POSIX_ACL_XATTR_ACCESS, POSIX_ACL_XATTR_ACCESS_LEN + 1 ) )
      || ( POSIX_ACL_XATTR_DEFAULT_LEN == name_len
        && 0 == memcmp( name, POSIX_ACL_XATTR_DEFAULT, POSIX_ACL_XATTR_DEFAULT_LEN + 1 ) ) ) {
    err = sbi->options.acl
      ? ufsd_xattr_set_acl( i, POSIX_ACL_XATTR_ACCESS_LEN == name_len? ACL_TYPE_ACCESS : ACL_TYPE_DEFAULT, value, size )
      : -EOPNOTSUPP;
  }
#endif
  else {
    err = ufsd_setxattr_hlp( i, name, value, size, flags, 0 );
  }
out:
  VfsTrace( -1, UFSD_LEVEL_XATTR, ("%s -> %d", hint, err ));
  return err;
}

#if defined UFSD_NTFS || defined UFSD_HFS
#if is_decl( INODE_OPS_XATTR_ANY )
///////////////////////////////////////////////////////////
// ufsd_removexattr
//
// inode_operations::removexattr
///////////////////////////////////////////////////////////
static int
ufsd_removexattr(
    IN struct dentry  *de,
    IN const char     *name
    )
{
#if is_decl( INODE_OPS_XATTR_V2 )
  return ufsd_setxattr( de, d_inode( de ), name, NULL, 0, XATTR_REPLACE );
#elif is_decl( INODE_OPS_XATTR_V1 )
  return ufsd_setxattr( de, name, NULL, 0, XATTR_REPLACE );
#elif !is_decl( INODE_OPS_XATTR_ANY )
  return ufsd_setxattr( NULL, de, name, NULL, 0, XATTR_REPLACE );
#else
  #error "Unknown version of {set,get}xattr"
#endif
}
#endif
#else
#define ufsd_removexattr NULL
#endif


#if defined UFSD_NTFS || defined UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_listxattr
//
// inode_operations::listxattr
//
// Copy a list of attribute names into the buffer
// provided, or compute the buffer size required.
// buffer is NULL to compute the size of the buffer required.
//
// Returns a negative error number on failure, or the number of bytes
// used / required on success.
///////////////////////////////////////////////////////////
static ssize_t
ufsd_listxattr(
    IN  struct dentry *de,
    OUT char          *buffer,
    IN  size_t        size
    )
{
  struct inode *i = de->d_inode;
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  ssize_t ret;
  int err;

  VfsTrace( +1, UFSD_LEVEL_XATTR, ("listxattr: r=%lx, %p, %zu, flags = %lx", i->i_ino, buffer, size, u->flags ));

  if ( unlikely( is_fork( u ) ) ) {
    ret = -EOPNOTSUPP;
    goto out;
  }

  lock_ufsd( sbi );

  err = ufsd_open_by_id( sbi, i );
  if ( likely( 0 == err ) )
    err = ufsdapi_list_xattr( sbi->ufsd, u->ufile, buffer, size, (size_t*)&ret );
  if ( 0 != err )
    ret = err;

  unlock_ufsd( sbi );

out:
  VfsTrace( -1, UFSD_LEVEL_XATTR, ("listxattr -> %zd", ret ));
  return ret;
}


#endif // #if defined UFSD_NTFS || defined UFSD_HFS


///////////////////////////////////////////////////////////
// ufsd_lookup
//
// inode_operations::lookup
//
//  This routine is a callback used to load inode for a
//  direntry when this direntry was not found in dcache.
//
// dir - container inode for this operation.
//
// dentry - On entry contains name of the entry to find.
//          On exit should contain inode loaded.
//
// Return:
// struct dentry* - direntry in case of one differs from one
//     passed to me. I return NULL to indicate original direntry has been used.
//     ERRP() can also be returned to indicate error condition.
//
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_lookup(
    IN struct inode  *dir,
    IN struct dentry *de
#if is_decl( INOP_LOOKUP_V2 )
  , IN struct nameidata *nd
#elif is_decl( INOP_LOOKUP_V3 )
  , IN unsigned int nd
#endif
    )
{
  struct inode *i = ufsd_create_or_open( dir, de, NULL );

  if ( IS_ERR( i ) ) {
    if ( -ENOENT == PTR_ERR( i ) )
      i = NULL;
    else
      return (struct dentry*)i;
  }

  if ( NULL != i && UFSD_SB( dir->i_sb )->options.use_dop ) {
    struct dentry *a = d_find_alias( i );
    if ( NULL != a ) {
      assert( 0 ); // to debug
      if ( !IS_ROOT(a) && !FlagOn( a->d_flags, DCACHE_DISCONNECTED ) ) {
        BUG_ON(d_unhashed(a));
        if (!S_ISDIR(i->i_mode))
          d_move( a, de );
        iput( i );
        return a;
      }
      dput(a);
    }
  }

  return d_splice_alias( i, de );
}


#if (defined UFSD_EXFAT || defined UFSD_REFS || defined UFSD_REFS3 || defined UFSD_FAT) && !defined UFSD_NTFS && !defined UFSD_HFS
  #define ufsd_link NULL
#else
///////////////////////////////////////////////////////////
// ufsd_link
//
// This function creates a hard link
// inode_operations::link
///////////////////////////////////////////////////////////
static int
ufsd_link(
    IN struct dentry  *ode,
    IN struct inode   *dir,
    OUT struct dentry *de
    )
{
  int err;
  struct inode *i;
  struct inode *oi = ode->d_inode;
  ucreate  cr = { (ufsd_file*)oi };

  assert( NULL != UFSD_FH(dir) );
  assert( S_ISDIR( dir->i_mode ) );
  assert( dir->i_sb == oi->i_sb );

  VfsTrace( +1, Dbg, ("link: r=%lx, \"%.*s\" => r=%lx, /\"%.*s\"",
                        oi->i_ino, (int)ode->d_name.len, ode->d_name.name,
                        dir->i_ino, (int)de->d_name.len, de->d_name.name ));

  if ( unlikely( is_fork( UFSD_U( oi ) ) ) ) {
    err = -EPERM;
  } else if ( IS_ERR( i = ufsd_create_or_open( dir, de, &cr ) ) ) {
    err = PTR_ERR( i );
  } else {
    err = 0;
    //
    // Hard link is created
    //
    assert( i == oi );
    d_instantiate( de, i );
    inc_nlink( i );
  }

  VfsTrace( -1, Dbg, ("link -> %d", err ));

  return err;
}
#endif

static int
ufsd_symlink(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN const char     *symname
    );

static int
ufsd_mknod(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode,
    IN dev_t          rdev
    );


static const struct inode_operations ufsd_dir_inode_operations = {
//  .getattr      = ufsd_getattr,
  .lookup       = ufsd_lookup,
  .create       = ufsd_create,
  .link         = ufsd_link,
  .unlink       = ufsd_unlink,
  .symlink      = ufsd_symlink,
  .mkdir        = ufsd_mkdir,
  .rmdir        = ufsd_unlink,
  .mknod        = ufsd_mknod,
  .rename       = ufsd_rename,
  .setattr      = ufsd_setattr,
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
  .permission   = ufsd_permission,
#if is_struct( INODE_OPERATIONS_GET_ACL )
  .get_acl      = ufsd_get_acl,
#endif
#if is_struct( INODE_OPERATIONS_SET_ACL )
  .set_acl      = ufsd_set_acl,
#endif
#endif
#endif
};


#if !is_decl( INODE_OPS_XATTR_ANY )
static bool
ufsd_xattr_user_list(struct dentry *dentry)
{
  return 1;
}


const struct xattr_handler ufsd_xattr_handler = {
  .prefix = "",
  .get    = ufsd_getxattr,
  .set    = ufsd_setxattr,
  .list   = ufsd_xattr_user_list,
};

const struct xattr_handler *ufsd_xattr_handlers[] = {
  &ufsd_xattr_handler,
  NULL
};
#endif

static const struct inode_operations ufsd_special_inode_operations = {
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
  .permission   = ufsd_permission,
#if is_struct( INODE_OPERATIONS_GET_ACL )
  .get_acl      = ufsd_get_acl,
#endif
#if is_struct( INODE_OPERATIONS_SET_ACL )
  .set_acl      = ufsd_set_acl,
#endif
#endif
#endif
  .setattr      = ufsd_setattr,
};


///////////////////////////////////////////////////////////
// ufsd_mknod
//
// ufsd_dir_inode_operations::mknod
///////////////////////////////////////////////////////////
static int
ufsd_mknod(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode,
    IN dev_t          rdev
    )
{
  struct inode *i;
  int     err;
  unsigned int udev32 = new_encode_dev( rdev );
  ucreate  cr = { NULL, &udev32, sizeof(udev32), 0, 0, mode };

  VfsTrace( +1, Dbg, ("mknod m=%o, %x", mode, udev32));

  if ( IS_ERR( i = ufsd_create_or_open( dir, de, &cr ) ) )
    err = PTR_ERR( i );
  else {
    err = 0;
    init_special_inode( i, i->i_mode, rdev );
    i->i_op = &ufsd_special_inode_operations;
    mark_inode_dirty( i );
    d_instantiate( de, i );
  }

  VfsTrace( -1, Dbg, ("mknod -> %d", err));

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_extend_initialized_size
//
// helper function 'i->i_mutex' / 'i->i_rwsem' is locked
///////////////////////////////////////////////////////////
static int
ufsd_extend_initialized_size(
    IN struct file    *file,
    IN unode          *u,
    IN const loff_t   valid,
    IN const loff_t   new_valid
    )
{
  int err;
  loff_t i_size = u->i.i_size;
  loff_t pos    = valid;
  struct address_space* mapping = u->i.i_mapping;

  assert( !is_sparsed_or_compressed( u ) );
  assert( Inode_is_locked( &u->i ) );

  DebugTrace( +1, Dbg, ("zero: r=%lx, [%llx-%llx,%llx]", u->i.i_ino, valid, new_valid, i_size ));

  BUG_ON( valid >= new_valid );

  for ( ;; ) {
    unsigned zerofrom = pos & ~PAGE_MASK;
    unsigned len      = PAGE_SIZE - zerofrom;
    struct page *page;
    void *fsdata;

#ifdef UFSD_USE_BD_ZERO
    if ( 0 == zerofrom )
      break;
#endif

    if ( pos + len > new_valid )
      len = new_valid - pos;

    err = pagecache_write_begin( file, mapping, pos, len, AOP_FLAG_UNINTERRUPTIBLE, &page, &fsdata );
    if ( err )
      goto error;

    zero_user_segment( page, zerofrom, PAGE_SIZE );

    err = pagecache_write_end( file, mapping, pos, len, len, page, fsdata );
    if ( err < 0 )
      goto error;
    BUG_ON( err != len );

    pos += len;
    if ( pos >= new_valid )
      break;

    balance_dirty_pages_ratelimited( mapping );
  }

#ifdef UFSD_USE_BD_ZERO
  if ( pos < new_valid ) {
    mapinfo map;
    usuper *sbi = UFSD_SB( u->i.i_sb );

    //
    // Align on cluster boundary
    //
    const loff_t to = (new_valid + sbi->cluster_mask) & sbi->cluster_mask_inv;

    do {
      loff_t tozero;

      //
      // map to read
      //
      err = vbo_to_lbo( sbi, u, pos, 0, &map );
      if ( unlikely( 0 != err ) ) {
        DebugTrace( 0, 0, ("zero: no map(%d) r=%lx, o=%llx s=%llx", err, u->i.i_ino, pos, i_size ));
        goto error;
      }

      if ( 0 == map.len )
        break;

      tozero = to - pos;
      if ( tozero > map.len )
        tozero = map.len;

      //
      // Zeroing (may be a long operation) ...
      //
      err = ufsd_bd_zero( u->i.i_sb, map.lbo, tozero );
      if ( unlikely( 0 != err ) )
        goto error;

      pos += tozero;
    } while( pos < to );

    set_valid_size( u, new_valid );
  }
#endif

  assert( i_size == u->i.i_size );
  assert( new_valid == u->valid );
  mark_inode_dirty( &u->i );
  DebugTrace( -1, Dbg, ("zero: r=%lx, -> sz=%llx,%llx", u->i.i_ino, u->valid, u->i.i_size ));

  return 0;

error:
  set_valid_size( u, valid );
  DebugTrace( -1, Dbg, ("zero: r=%lx, -> error %d, [%llx-%llx,%llx]", u->i.i_ino, err, valid, new_valid, i_size ));
  ufsd_printk( u->i.i_sb, "failed to extend initialized size of inode 0x%lx (error %d), [%llx-%llx,%llx].", u->i.i_ino, err, valid, new_valid, i_size );
  return err;
}


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_file_stream:
//
// Helper function to write/read ntfs streams
///////////////////////////////////////////////////////////
noinline static ssize_t
ufsd_file_stream(
    IN OUT struct kiocb     *iocb,
    IN const struct iovec   *iov,
    IN unsigned long        nr_segs,
    IN loff_t               pos,
    IN size_t               count,
    IN struct file          *file,
    IN unode                *u,
    IN const unsigned char  *p,
    IN int                   op
    )
{
  usuper       *sbi     = UFSD_SB( u->i.i_sb );
  struct qstr  *s       = &file_dentry(file)->d_name;
  int           len     = s->name + s->len - p;
  ssize_t       err     = 0;
  ssize_t       bytes   = 0; // written/read
  unsigned long seg;

  DEBUG_ONLY( const char *hint = READ == op? "read":"write"; )

  DebugTrace( +1, Dbg, ("file_stream_%s: r=%lx, (:%.*s), %llx, %zx", hint, u->i.i_ino, len, p, pos, count ));

  if ( unlikely( IS_IO_DIRECT( iocb, file ) ) ) {
    DebugTrace( 0, Dbg, ("does not support direct I/O for streams" ));
    err = -EOPNOTSUPP;
    goto out;
  }

  //
  // Operate 'count' bytes via sbi->rw_buffer
  //
  lock_ufsd( sbi );

  assert( NULL != sbi->rw_buffer );

  for ( seg = 0; seg < nr_segs && bytes < count; seg++ ) {
    char __user *buf = iov[seg].iov_base;
    size_t iov_len  = min_t( size_t, iov[seg].iov_len, count - bytes );

    while( 0 != iov_len ) {

      size_t to_rdwr  = min_t( size_t, RW_BUFFER_SIZE - (((size_t)pos) & ( RW_BUFFER_SIZE - 1 )), iov_len );
      size_t rdwr; // read/written bytes

      if ( READ == op ) {
        err = ufsdapi_file_read( sbi->ufsd, u->ufile, p, len, pos, to_rdwr, sbi->rw_buffer, &rdwr );
        if ( 0 == err && 0 != copy_to_user( buf, sbi->rw_buffer, rdwr ) ) {
          err = -EFAULT;
          goto end_cycle;
        }
        if ( 0 != err )
          goto end_cycle;
      } else {
        if ( copy_from_user( sbi->rw_buffer, buf, to_rdwr ) ) {
          err = -EFAULT;
          goto end_cycle;
        }
        err = ufsdapi_file_write( sbi->ufsd, u->ufile, p, len, pos, to_rdwr, sbi->rw_buffer, &rdwr );
        if ( 0 != err )
          goto end_cycle;

        if ( rdwr != to_rdwr ) {
          err = -EIO;     // ??
          goto end_cycle;
        }
      }

      bytes   += rdwr;
      pos     += rdwr;
      buf     += rdwr;
      iov_len -= rdwr;

      if ( READ == op && rdwr < to_rdwr )
        break;

    } // while( 0 != iov_len )
  } // for ( seg = 0; seg < nr_segs && bytes < count; seg++ )

end_cycle:
  unlock_ufsd( sbi );

  // Update file position
  iocb->ki_pos = pos;

  if ( unlikely( 0 != bytes ) )
    err = bytes;

out:
  DebugTrace( -1, Dbg, ("file_stream_%s => %zx", hint, err ));

  return err;
}
#endif // #ifdef UFSD_NTFS


#if is_decl( GENERIC_FILE_READ_ITER_V1 )

#if defined UFSD_NTFS || defined UFSD_TRACE
///////////////////////////////////////////////////////////
// ufsd_file_read_iter:  3.16+
//
// based on: mm/filemap.c: generic_file_read_iter
// file_operations::read_iter
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_read_iter(
    IN struct kiocb     *iocb,
    IN struct iov_iter  *iter
    )
{
  ssize_t err;
  struct file  *file = iocb->ki_filp;
  struct inode *i    = file->f_mapping->host;
  unode        *u    = UFSD_U( i );

#ifdef UFSD_NTFS
  const unsigned char* p;

  if ( unlikely( is_encrypted( u ) ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_read: r=%lx, attempt to read encrypted file", i->i_ino ));
    return -ENOSYS;
  }
#endif

  VfsTrace( +1, Dbg, ("file_read: r=%lx, [%llx + %zx), sz=%llx,%llx%s",
                       i->i_ino, iocb->ki_pos, iov_length( iter->iov, iter->nr_segs ), u->valid, i->i_size,
                       IS_IO_DIRECT( iocb, file )? ",di":"" ));

#ifdef UFSD_NTFS
  if ( unlikely( NULL != ( p = is_stream( file ) ) ) ) {
    err = ufsd_file_stream( iocb, iter->iov, iter->nr_segs, iocb->ki_pos, iov_iter_count( iter ), file, u, p, READ );
  } else
#endif
  {
    err = generic_file_read_iter( iocb, iter );
  }

  VfsTrace( -1, Dbg, ("file_read -> %zx", err));

  return err;
}
#else
  #define ufsd_file_read_iter generic_file_read_iter
#endif

#if is_struct( ADDRESS_SPACE_BACKING_DEV_INFO )
  #define  GET_BDI( __MAP, __SB )  __MAP->backing_dev_info
#elif is_decl( INODE_TO_BDI )
  // http://lxr.free-electrons.com/source/fs/fs-writeback.c?v=4.0#L81
  #define  GET_BDI( __MAP, __SB )  __SB->s_bdi
#endif


///////////////////////////////////////////////////////////
// ufsd_file_write_iter: 3.16+
//
// based on: mm/filemap.c: generic_file_write_iter
// file_operations::write_iter
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_write_iter(
    IN struct kiocb     *iocb,
    IN struct iov_iter  *iter
    )
{
  ssize_t ret;
  loff_t  end, i_size;
  struct timespec now;
  int dirty                     = 0;
  struct file  *file            = iocb->ki_filp;
  struct address_space *mapping = file->f_mapping;
  struct inode *i               = mapping->host;
  struct super_block  *sb       = i->i_sb;
  usuper  *sbi                  = UFSD_SB( sb );
  unode *u                      = UFSD_U( i );
  ssize_t written               = 0;
  loff_t  pos                   = iocb->ki_pos;
  size_t count                  = iov_iter_count( iter );

  if ( unlikely( !is_bdi_ok( sb ) ) )
    return -ENODEV;

  if ( unlikely( is_encrypted( u ) ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_write: r=%lx, attempt to write to encrypted file", i->i_ino ));
    return -ENOSYS;
  }

  VfsTrace( +1, Dbg, ("file_write: r=%lx, [%llx + %zx), sz=%llx,%llx%s%s",
                        i->i_ino, iocb->ki_pos, iov_length( iter->iov, iter->nr_segs ), u->valid, i->i_size,
                        IS_IO_APPEND( iocb, file )? ",append":"",
                        IS_IO_DIRECT( iocb, file )? ",di":"" ));

  Inode_lock( i );

#if is_decl( GENERIC_WRITE_CHECKS_V1 )
  // [3.16 - 4.1)
  ret = generic_write_checks( file, &pos, &count, S_ISBLK( i->i_mode ) );
  if ( unlikely( ret || 0 == count ) )
    goto unlock;

  iov_iter_truncate( iter, count );

#elif is_decl( GENERIC_WRITE_CHECKS_V2 )
  // 4.1+
  ret = generic_write_checks( iocb, iter );
  if ( unlikely( ret <= 0 ) )
    goto unlock;

#endif

  // We can write back this queue in page reclaim
  current->backing_dev_info = GET_BDI( mapping, sb );

  ret = file_remove_privs( file );
  if ( unlikely( ret ) )
    goto out;

#ifdef UFSD_NTFS
  {
    const unsigned char* p = is_stream( file );
    if ( unlikely( NULL != p ) ) {
      ret = ufsd_file_stream( iocb, iter->iov, iter->nr_segs,
                              IS_IO_APPEND( iocb, file )? u->i.i_size : iocb->ki_pos,
                              iov_iter_count( iter ), file, u, p, WRITE );
      goto out;
    }
  }
#endif // #ifdef UFSD_NTFS

  i_size  = i->i_size;
  end     = pos + count;

  if ( likely( end > i_size ) ) {

    //
    // Change in-memory size first
    //
    i_size_write( i, end );

    //
    // Change on-disk size second
    //
    if ( is_sparsed_or_compressed( u ) ) {
      ret = ufsd_set_size_hlp( i, i_size, end );
      if ( unlikely( 0 != ret ) )
        goto out;
    } else {
      mapinfo map;
      ret = vbo_to_lbo( sbi, u, pos, count, &map );
      if ( unlikely( 0 != ret  ) ) {
        i_size_write( i, i_size );
        goto out;
      }
    }
    dirty = 1;
  }

  if ( !is_sparsed_or_compressed( u ) ) {
    loff_t valid = get_valid_size( u, NULL, NULL );
    if ( unlikely( pos > valid ) ) {
      ret = ufsd_extend_initialized_size( file, u, valid, pos );
      if ( unlikely( ret < 0 ) )
        goto out;
      dirty = 1;
    }
  }

  //
  // Update inode times and mark node for dirty if necessary
  //
  now = ufsd_inode_current_time( sbi );

  if ( i->i_mtime.tv_sec != now.tv_sec || i->i_mtime.tv_nsec != now.tv_nsec ) {
    i->i_mtime = now;
    dirty = 1;
  }

  if ( i->i_ctime.tv_sec != now.tv_sec || i->i_ctime.tv_nsec != now.tv_nsec ) {
    i->i_ctime = now;
    dirty = 1;
  }

  if ( dirty )
    mark_inode_dirty_sync( i );

  //
  // coalesce the iovecs and go direct-to-BIO for O_DIRECT
  //
#if is_decl( GENERIC_WRITE_CHECKS_V1 )
  // [3.16 - 4.1)
  if ( unlikely( IS_IO_DIRECT( iocb, file ) ) ) {
    loff_t endbyte;
    ssize_t status;

    written = generic_file_direct_write( iocb, iter, pos );
    if ( unlikely( written < 0 || written == count || IS_DAX( i ) ) )
      goto out;

    DebugTrace( 0, Dbg, ("**** switch to generic_perform_write: %zx", written ));

    pos   += written;
//    count -= written; // no more used below

    status = generic_perform_write( file, iter, pos );
    if ( unlikely( status < 0 && !written ) ) {
      ret = status;
      goto out;
    }
    iocb->ki_pos = pos + status;
    endbyte      = iocb->ki_pos - 1;

    ret = filemap_write_and_wait_range( mapping, pos, endbyte );
    if ( 0 == ret ) {
      written += status;
      invalidate_mapping_pages( mapping, pos >> PAGE_SHIFT, endbyte >> PAGE_SHIFT );
    }
  } else {

    written = generic_perform_write( file, iter, pos );
    if ( likely( written >= 0 ) )
      iocb->ki_pos = pos + written;
  }
#else
  // 4.1+
  if ( unlikely( IS_IO_DIRECT( iocb, file ) ) ) {
    loff_t pos, endbyte, status;

#if is_decl( GENERIC_FILE_DIRECT_WRITE_V2 ) // 4.7+
    written = generic_file_direct_write( iocb, iter );
#elif is_decl( GENERIC_FILE_DIRECT_WRITE_V1 ) // 4.1+..4.6-
    written = generic_file_direct_write( iocb, iter, iocb->ki_pos );
#else
    #error "Unknown version of generic_file_direct_write"
#endif

    if ( unlikely( written < 0 || !iov_iter_count( iter ) || IS_DAX( i ) ) )
      goto out;

    status = generic_perform_write( file, iter, pos = iocb->ki_pos );
    if ( unlikely( status < 0 ) ) {
      ret = status;
      goto out;
    }

    //
    // We need to ensure that the page cache pages are written to
    // disk and invalidated to preserve the expected O_DIRECT
    // semantics.
    //
    endbyte = pos + status - 1;

    ret = filemap_write_and_wait_range( mapping, pos, endbyte );
    if ( 0 == ret ) {
      iocb->ki_pos = endbyte + 1;
      written += status;
      invalidate_mapping_pages( mapping, pos >> PAGE_SHIFT, endbyte >> PAGE_SHIFT );
    } else {
      //
      // We don't know how much we wrote, so just return
      // the number of bytes which were direct-written
      //
    }
  } else {

    written = generic_perform_write( file, iter, iocb->ki_pos );
    if ( likely( written > 0 ) )
      iocb->ki_pos += written;
  }
#endif

out:
  current->backing_dev_info = NULL;
  if ( unlikely( 0 != written ) ) {
    ret = written;
#ifdef Try_to_writeback_inodes_sb
    if ( unlikely( sbi->options.wb && written >= PAGE_SIZE ) ) {
      if ( atomic_dec_and_test( &sbi->writeiter_cnt ) ) {
        Try_to_writeback_inodes_sb( sb );
        atomic_set( &sbi->writeiter_cnt, sbi->options.wb );
      }
    } else if ( unlikely ( sbi->options.wbMb_in_pages ) ) {
      // Check if the number of pages exceeds the limit
      unsigned dirty_pages_count = atomic_read( &sbi->dirty_pages_count );
      if ( dirty_pages_count >= sbi->options.wbMb_in_pages ) {
        // Need this debug print for test_wbxm_option
        DebugTrace( 0, Dbg, ( "Call to Try_to_writeback_inodes_sb (%x)", dirty_pages_count ) );
        Try_to_writeback_inodes_sb( i->i_sb );
      }
    }
#endif
  }

unlock:
  Inode_unlock( i );

  if ( likely( ret > 0 ) ) {
#if is_decl( GENERIC_WRITE_SYNC_V2 ) // 4.7+
    ret = generic_write_sync( iocb, ret );
#elif is_decl( GENERIC_WRITE_SYNC_V1 ) // 4.6-
    ssize_t err = generic_write_sync( file, iocb->ki_pos - ret, ret );
    if ( err < 0 )
      ret = err;
#endif
  }

  VfsTrace( -1, Dbg, ("file_write => %zx", ret ));

  return ret;
}

#else

// Turn on helper define
#define UFSD_USE_AIO_READ_WRITE

#if defined UFSD_NTFS || defined UFSD_TRACE
///////////////////////////////////////////////////////////
// ufsd_file_aio_read, 3.15-
//
// based on: mm/filemap.c __generic_file_aio_read
// file_operations::aio_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_read(
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN unsigned long       nr_segs,
    IN loff_t              pos
    )
{
  ssize_t err;
  struct file *file = iocb->ki_filp;
  struct inode *i   = file->f_mapping->host;
  unode *u          = UFSD_U( i );

#ifdef UFSD_NTFS
  const unsigned char* p;

  if ( unlikely( is_encrypted( u ) ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_read: r=%lx, attempt to read encrypted file", i->i_ino ));
    return -ENOSYS;
  }
#endif

  VfsTrace( +1, Dbg, ("file_read: r=%lx, [%llx + %zx), sz=%llx,%llx%s",
                       i->i_ino, pos, iov_length( iov, nr_segs ), u->valid, i->i_size,
                       IS_IO_DIRECT(iocb, file)?",di":"" ));

#ifdef UFSD_NTFS
  if ( unlikely( NULL != ( p = is_stream( file ) ) ) ) {
    // Read stream
    size_t count = 0;
    err   = generic_segment_checks( iov, &nr_segs, &count, VERIFY_WRITE );
    if ( likely( 0 == err ) )
      err = ufsd_file_stream( iocb, iov, nr_segs, pos, count, file, u, p, READ );

  } else
#endif // #ifdef UFSD_NTFS

  {
    err = generic_file_aio_read( iocb, iov, nr_segs, pos );
  }

  VfsTrace( -1, Dbg, ("file_read -> %zx", err));

  return err;
}

#else
  #define ufsd_file_aio_read    generic_file_aio_read
#endif


#if !( is_decl( GENERIC_FILE_BUFFERED_WRITE ) )
// 3.15 only:
static ssize_t
generic_file_buffered_write(struct kiocb *iocb, const struct iovec *iov,
    unsigned long nr_segs, loff_t pos, loff_t *ppos,
    size_t count, ssize_t written)
{
  struct file *file = iocb->ki_filp;
  ssize_t status;
  struct iov_iter i;

  iov_iter_init(&i, iov, nr_segs, count, written);
  status = generic_perform_write(file, &i, pos);

  if (likely(status >= 0)) {
    written += status;
    *ppos = pos + status;
  }

  return written ? written : status;
}

// ppos is not used
#define generic_file_direct_write( iocb, iov, nr_segs, pos, ppos, count, ocount ) generic_file_direct_write( iocb, iov, nr_segs, pos, count, ocount )
#endif


///////////////////////////////////////////////////////////
// ufsd_file_aio_write: 3.15-
//
// based on 'mm\filemap.c' generic_file_aio_write
// file_operations::aio_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_write(
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN unsigned long      nr_segs,
    IN loff_t             pos
    )
{
  ssize_t ret;
  loff_t i_size, end;
  struct timespec now;
  struct file *file             = iocb->ki_filp;
  struct address_space *mapping = file->f_mapping;
  struct inode *i               = mapping->host;
  struct super_block  *sb       = i->i_sb;
  usuper* sbi                   = UFSD_SB( i->i_sb );
  unode *u                      = UFSD_U( i );
  size_t  count, ocount         = 0;
  ssize_t written               = 0;
  int dirty                     = 0;

#ifdef UFSD_CHECK_TIME
  unsigned long j0  = jiffies;
  unsigned long j1  = 0;
  unsigned long j2  = 0;
  unsigned long j3  = 0;
  unsigned long j4  = 0;
  unsigned long j5  = 0;
  loff_t valid      = u->valid;
  loff_t isize      = i->i_size;
  loff_t asize      = inode_get_bytes( i );
  size_t len        = iov_length( iov, nr_segs );
#endif

  BUG_ON( iocb->ki_pos != pos );

  if ( unlikely( !is_bdi_ok( sb ) ) )
    return -ENODEV;

  if ( unlikely( is_encrypted( u ) ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_write: r=%lx, attempt to write to encrypted file", i->i_ino ));
    return -ENOSYS;
  }

  VfsTrace( +1, Dbg, ("file_write: r=%lx, [%llx + %zx), sz=%llx,%llx%s%s",
                       i->i_ino, pos, iov_length( iov, nr_segs ), u->valid, i->i_size,
                       IS_IO_APPEND(iocb, file)? ",append":"",
                       IS_IO_DIRECT(iocb, file)?",di":"" ));

  Inode_lock( i );

  ret = generic_segment_checks( iov, &nr_segs, &ocount, VERIFY_READ );
  if ( unlikely( ret ) )
    goto unlock;

  count = ocount;

#ifdef vfs_check_frozen
  // 3.5--
  vfs_check_frozen( sb, SB_FREEZE_WRITE );
#endif

  // We can write back this queue in page reclaim
  current->backing_dev_info = mapping->backing_dev_info;

  ret = generic_write_checks( file, &pos, &count, S_ISBLK( i->i_mode ) );
  if ( unlikely( ret || 0 == count ) )
    goto out;

  ret = file_remove_privs( file );
  if ( unlikely( ret ) )
    goto out;

#ifdef UFSD_NTFS
  {
    const unsigned char* p = is_stream( file );
    if ( unlikely( NULL != p ) ) {
      // Write stream
      ret = ufsd_file_stream( iocb, iov, nr_segs,
                              IS_IO_APPEND( iocb, file )? u->i.i_size : pos,
                              count, file, u, p, WRITE );
      goto out;
    }
  }
#endif

  i_size  = i->i_size;
  end     = pos + count;

  if ( likely( end > i_size ) ) {
    //
    // Change in-memory size first
    //
    i_size_write( i, end );

    //
    // Change on-disk size second
    //
    if ( is_sparsed_or_compressed( u ) ) {
      ret = ufsd_set_size_hlp( i, i_size, end );
      if ( unlikely( 0 != ret ) )
        goto out;
    } else {
      mapinfo map;
      ret = vbo_to_lbo( sbi, u, pos, count, &map );
      if ( unlikely( 0 != ret  ) ) {
        i_size_write( i, i_size );
        goto out;
      }
    }
    dirty = 1;
  }

  if ( !is_sparsed_or_compressed( u ) ) {
    loff_t valid = get_valid_size( u, NULL, NULL );
    if ( unlikely( pos > valid ) ) {
      CHECK_TIME_ONLY( unsigned long jt = jiffies; )
      ret = ufsd_extend_initialized_size( file, u, valid, pos );
      CHECK_TIME_ONLY( j2 = jiffies - jt; )
      if ( unlikely( ret < 0 ) )
        goto out;
      dirty = 1;
    }
  }

  //
  // Update inode times and mark node for dirty if necessary
  //
  now = ufsd_inode_current_time( sbi );

  if ( i->i_mtime.tv_sec != now.tv_sec || i->i_mtime.tv_nsec != now.tv_nsec ) {
    i->i_mtime = now;
    dirty = 1;
  }

  if ( i->i_ctime.tv_sec != now.tv_sec || i->i_ctime.tv_nsec != now.tv_nsec ) {
    i->i_ctime = now;
    dirty = 1;
  }

  if ( dirty )
    mark_inode_dirty_sync( i );

  //
  // coalesce the iovecs and go direct-to-BIO for O_DIRECT
  //
  if ( unlikely( file->f_flags & O_DIRECT ) ) {
    //
    // First try common direct i/o
    //
    written = generic_file_direct_write( iocb, iov, &nr_segs, pos, &iocb->ki_pos, count, ocount );

    if ( unlikely( written >= 0 && written != count ) ) {
      ssize_t written_buffered;
      DebugTrace( 0, Dbg, ("**** switch to generic_file_buffered_write: %zx", written ));

      pos   += written;
      count -= written;
      written_buffered = generic_file_buffered_write( iocb, iov, nr_segs, pos, &iocb->ki_pos, count, written );
      if ( unlikely( written_buffered < 0 ) ) {
        ret = written_buffered;
      } else {
        //
        // We need to ensure that the page cache pages are written to
        // disk and invalidated to preserve the expected O_DIRECT semantics.
        //
        ssize_t endbyte = pos + written_buffered - written - 1;

#ifdef generic_file_direct_write
        iocb->ki_pos = pos + written_buffered;
#endif

        ret = filemap_write_and_wait_range( mapping, pos, endbyte );
        if ( 0 == ret ) {
          written = written_buffered;
          invalidate_mapping_pages( mapping, pos >> PAGE_SHIFT, endbyte >> PAGE_SHIFT );
        } else {
          //
          // We don't know how much we wrote, so just return
          // the number of bytes which were direct-written
          //
        }
      }
    }
  } else {
    CHECK_TIME_ONLY( unsigned long jt = jiffies; )
    written = generic_file_buffered_write( iocb, iov, nr_segs, pos, &iocb->ki_pos, count, 0 );
    CHECK_TIME_ONLY( j3 = jiffies - jt; )
  }

out:
  current->backing_dev_info = NULL;
  if ( unlikely( 0 != written ) ) {
    ret = written;
#ifdef Try_to_writeback_inodes_sb
    if ( unlikely( sbi->options.wb && written >= PAGE_SIZE ) ) {
      if ( atomic_dec_and_test( &sbi->writeiter_cnt ) ) {
        Try_to_writeback_inodes_sb( i->i_sb );
        atomic_set( &sbi->writeiter_cnt, sbi->options.wb );
      }
    } else if ( unlikely ( sbi->options.wbMb_in_pages ) ) {
      // Check if the number of pages exceeds the limit
      unsigned dirty_pages_count = atomic_read( &sbi->dirty_pages_count );
      if ( dirty_pages_count >= sbi->options.wbMb_in_pages ) {
        // Need this debug print for test_wbxm_option
        DebugTrace( 0, Dbg, ( "Call to Try_to_writeback_inodes_sb (%x)", dirty_pages_count ) );
        Try_to_writeback_inodes_sb( i->i_sb );
      }
    }
#endif
  }

  CheckTimeEx( 2, "%u, %u, %u", jiffies_to_msecs(j1), jiffies_to_msecs(j2), jiffies_to_msecs(j3) );

unlock:
  Inode_unlock( i );
  CHECK_TIME_ONLY( j4 = jiffies - j0; )

  if ( likely( ret > 0 ) ) {//|| EIOCBQUEUED == ret ) ) {
    CHECK_TIME_ONLY( unsigned long jt = jiffies; )
    ssize_t err = generic_write_sync( file, pos, ret );
    CHECK_TIME_ONLY( j5 = jiffies - jt; )
    if ( err < 0 )//&& ret > 0 )
      ret = err;
  }

  VfsTrace( -1, Dbg, ("file_write => %zx", ret ));

#ifdef UFSD_CHECK_TIME
  if ( jiffies - j0 > 2*HZ ){
    unsigned int dt = jiffies_to_msecs( jiffies - j0 );
    DebugTrace( 0, 0, ("**** aio_write: %u, %u, %u, %s, r=%lx, [%llx + %zx), sz=%llx,%llx,%llx -> %llx,%llx,%llx",
                dt, jiffies_to_msecs(j4), jiffies_to_msecs(j5),
                current->comm, i->i_ino, pos, len,
                valid, isize, asize,
                u->valid, i->i_size, inode_get_bytes( i ) ));
    if ( dt > 5000 )
      dump_stack();
  }
#endif

  return ret;
}
#endif // #if is_decl( GENERIC_FILE_READ_ITER_V1 )


///////////////////////////////////////////////////////////
// ufsd_file_mmap
//
// file_operations::mmap
///////////////////////////////////////////////////////////
static int
ufsd_file_mmap(
    IN struct file                *file,
    IN OUT struct vm_area_struct  *vma
    )
{
  int err;
  struct inode *i   = file_inode( file );
  unode *u          = UFSD_U( i );
  UINT64 from       = ((UINT64)vma->vm_pgoff << PAGE_SHIFT);

  assert( from < i->i_size );

  VfsTrace( +1, Dbg, ("file_mmap: r=%lx, %lx(%s%s), [%llx, %llx), s=%llx,%llx",
              i->i_ino, vma->vm_flags,
              (vma->vm_flags & VM_READ)?"r":"",
              (vma->vm_flags & VM_WRITE)?"w":"",
              from, from + vma->vm_end - vma->vm_start, u->valid, i->i_size ));

  if ( unlikely( is_stream( file ) || is_encrypted( u ) ) ) {
    err = -EOPNOTSUPP; // no mmap for streams and encrypted files
    goto out;
  }

  if ( vma->vm_flags & VM_WRITE ) {
    loff_t i_size;
    UINT64 valid = get_valid_size( u, &i_size, NULL );

    from += vma->vm_end - vma->vm_start;
    if ( from > i_size )
      from = i_size;

    if ( valid < from ) {
      if ( is_sparsed_or_compressed( u ) ) {
        DebugTrace( 0, Dbg, ("file_mmap - update valid size for sparsed file %llx -> %llx", valid, from ));
        set_valid_size( u, from );
      } else {
        Inode_lock( i );
        err = ufsd_extend_initialized_size( file, u, valid, from );
        Inode_unlock( i );
        if ( unlikely( 0 != err ) )
          goto out;
      }
    }
  }

  //
  // Call generic function
  //
  err = generic_file_mmap( file, vma );

out:
  VfsTrace( -1, Dbg, ("file_mmap -> %d", err) );

  return err;
}


#if defined UFSD_TRACE || defined UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_file_splice_read
//
// file_operations::splice_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_read(
    IN struct file  *file,
    IN OUT loff_t   *ppos,
    IN struct pipe_inode_info *pipe,
    IN size_t       len,
    IN unsigned int flags
    )
{
  ssize_t ret;
  VfsTrace( +1, Dbg, ("file_splice_read: r=%lx, %llx %zx", file_inode( file )->i_ino, *ppos, len ));

  ret = is_stream( file )? -ENOSYS : generic_file_splice_read( file, ppos, pipe, len, flags );

  VfsTrace( -1, Dbg, ("file_splice_read -> %zx", ret));

  return ret;
}
#else
  #define ufsd_file_splice_read generic_file_splice_read
#endif


#if is_decl( GENERIC_FILE_SPLICE_WRITE )
#if defined UFSD_TRACE || defined UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_file_splice_write
//
// file_operations::splice_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_write(
    IN struct pipe_inode_info *pipe,
    IN struct file  *file,
    IN OUT loff_t   *ppos,
    IN size_t       len,
    IN unsigned int flags
    )
{
  ssize_t ret;
  struct inode *i = file_inode( file );

  if ( unlikely( !is_bdi_ok( i->i_sb ) ) )
    return -ENODEV;

  VfsTrace( +1, Dbg, ("file_splice_write: r=%lx, %llx %zx", i->i_ino, *ppos, len ));

  ret = is_stream( file )? -ENOSYS : generic_file_splice_write( pipe, file, ppos, len, flags );

  VfsTrace( -1, Dbg, ("file_splice_write -> %zx", ret));

  return ret;
}
#else
  #define ufsd_file_splice_write  generic_file_splice_write
#endif
#endif


#if is_struct( INODE_OPERATIONS_FALLOCATE )
  #define DECLARE_FALLOCATE_ARG     struct inode* i
  #define DECLARE_FALLOCATE_INODE
#elif is_struct( FILE_OPERATIONS_FALLOCATE )
  #define DECLARE_FALLOCATE_ARG     struct file*  file
  #define DECLARE_FALLOCATE_INODE   struct inode* i = file_inode( file );
#endif

#ifdef DECLARE_FALLOCATE_ARG
#include <linux/falloc.h>
///////////////////////////////////////////////////////////
// ufsd_fallocate
//
// inode_operations::fallocate
///////////////////////////////////////////////////////////
static long
ufsd_fallocate(
    IN DECLARE_FALLOCATE_ARG,
    IN int    mode,
    IN loff_t offset,
    IN loff_t len
    )
{
  int err;
  DECLARE_FALLOCATE_INODE
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  loff_t new_len = offset + len;
  mapinfo map;

  VfsTrace( +1, Dbg, ("fallocate: r=%lx, %llx, %llx, sz=%llx,%llx, mode=%d", i->i_ino, offset, len, u->valid, i->i_size, mode ));

  //
  // Check simple case
  //
  if ( new_len > i->i_sb->s_bdev->bd_inode->i_size ) {
    err = -ENOSPC;
  } else {

    loff_t i_size;

    //
    // Call UFSD library
    //
    Inode_lock( i );

    //
    // Change in-memory size before 'vbo_to_lbo'
    //
    i_size = i_size_read( i );
    if ( new_len > i_size )
      i_size_write( i, new_len );

    //
    // map to write
    //
    err = vbo_to_lbo( sbi, u, offset, len, &map );
    if ( 0 == err ) {
      assert( 0 != map.len );
      if ( new_len > i_size ) {
        if ( FlagOn( mode, FALLOC_FL_KEEP_SIZE ) )
          ufsd_printk( i->i_sb, "fallocate: ignore keep_size for inode 0x%lx.", i->i_ino );
      }
    } else {
      i_size_write( i, i_size );
    }

    Inode_unlock( i );
  }

  VfsTrace( -1, Dbg, ("fallocate -> %d, sz=%llx,%llx", err, u->valid, i->i_size ));

  return err;
}
#endif // #if is_struct( INODE_OPERATIONS_FALLOCATE )


#if is_struct( FILE_OPERATIONS_SPLICE_WRITE_FROM_SOCKET ) && defined CONFIG_BCM_RECVFILE
#include <linux/splice.h>
#include <linux/net.h>
#include <net/sock.h>
///////////////////////////////////////////////////////////
// ufsd_splice_write_from_socket
//
// file_operations::splice_write_from_socket
///////////////////////////////////////////////////////////
static ssize_t
ufsd_splice_write_from_socket(
    IN struct file        *file,
    IN struct socket      *socket,
    IN OUT loff_t __user  *off,
    IN size_t             count
    )
{
  struct address_space *mapping = file->f_mapping;
  struct inode *i = mapping->host;
#ifdef Try_to_writeback_inodes_sb
  usuper *sbi = UFSD_SB( i->i_sb );
#endif
  unode *u        = UFSD_U( i );
  struct page *page;
  loff_t pos, start_pos, valid;
  size_t idx, tmp, npages = 0;
  struct page **pages;
  struct kvec *iov;
  struct msghdr msg;
  long rcvtimeo;
  unsigned long flags;
  int dirty;
  int err;

  C_ASSERT( sizeof(struct iovec) == sizeof(struct kvec) );

  if ( unlikely( count > MAX_PAGES_PER_RECVFILE * PAGE_SIZE ) ) {
    printk( KERN_WARNING "%s: count(%zu) exceeds maxinum\n", __func__, count );
    return -EINVAL;
  }

  if ( unlikely( NULL == off ) )
    return -EINVAL;

  if ( unlikely( copy_from_user( &start_pos, off, sizeof(*off) ) ) )
    return -EFAULT;

  pos   = start_pos; // save original value

  pages = kmalloc( MAX_PAGES_PER_RECVFILE * sizeof(struct page*), GFP_KERNEL  );
  if ( unlikely( NULL == pages ) )
    return -ENOMEM;

  iov = kmalloc( MAX_PAGES_PER_RECVFILE * sizeof(struct kvec), GFP_KERNEL );
  if ( unlikely( NULL == iov ) ) {
    kfree( pages );
    return -ENOMEM;
  }

  DebugTrace( +1, Dbg, ("splice_write_from_socket: r=%lx, [%llx + %zx), sz=%llx,%llx", i->i_ino, pos, count, u->valid, i->i_size ));

  Inode_lock( i );

#ifdef vfs_check_frozen
  // 3.5--
  vfs_check_frozen( i->i_sb, SB_FREEZE_WRITE );
#endif

  dirty = 0;

  // We can write back this queue in page reclaim
  current->backing_dev_info = mapping->backing_dev_info;

  err = generic_write_checks( file, &pos, &count, S_ISBLK( i->i_mode ) );
  if ( unlikely( err || 0 == count ) )
    goto done;

  err = file_remove_privs( file );
  if ( unlikely( err ) )
    goto done;

  // Allocate space if necessary
  if ( unlikely( pos + count > i->i_size ) ) {
    err = ufsd_set_size_hlp( i, i->i_size, pos + count );
    if ( unlikely( 0 != err ) )
      goto done;

    dirty = 1;
  }

  valid = get_valid_size( u, NULL, NULL );
  if ( !is_sparsed_or_compressed( u ) ) {
    if ( unlikely( pos > valid ) ) {
      err = ufsd_extend_initialized_size( file, u, valid, pos );
      if ( unlikely( err < 0 ) )
        goto done;
      valid = pos;
    }
  }

  // Prepare iov structs
  idx = 0;      // number of pages
  tmp = count;  // save original 'count' to use later
  do {
    unsigned offset = pos & (PAGE_SIZE - 1);
    unsigned bytes  = PAGE_SIZE - offset;
    if ( bytes > count )
      bytes = count;

    if ( pos < valid && 0 != offset ) {
      page = read_mapping_page( mapping, pos >> PAGE_SHIFT, NULL );

      if ( unlikely( IS_ERR( page ) ) ) {
        err = PTR_ERR( page );
Error:
        ufsd_printk( i->i_sb, "Failed to read page at offset 0x%llx (error %d)", pos, err );
        goto unmap_all;
      } else if ( unlikely( PageError( page ) ) ) {
        err = -EIO;
        put_page( page );
        goto Error;
      }

      lock_page( page );

      if ( unlikely( page->mapping != mapping ) ) {
        unlock_page( page );
        put_page( page );
        continue;
      }
    } else {
      //pos >= valid || 0 == offset
      page = grab_cache_page_write_begin( mapping, pos >> PAGE_SHIFT, AOP_FLAG_UNINTERRUPTIBLE|AOP_FLAG_NOFS );
      if ( unlikely( NULL == page ) ) {
        ufsd_printk( i->i_sb, "failed to allocate page cache page for inode 0x%lx at start 0x%llx.", i->i_ino, pos );
        err = -ENOMEM;
        goto unmap_all;
      }
      // page is locked
    }

    pages[idx]        = page;
    iov[idx].iov_base = kmap( page ) + offset;
    iov[idx].iov_len  = bytes;
    idx              += 1;
    count            -= bytes;
    pos              += bytes;
  } while ( 0 != count );

  rcvtimeo = socket->sk->sk_rcvtimeo;
  socket->sk->sk_rcvtimeo = 8 * HZ;

  //
  // NOTES:
  // - inside 'kernel_recvmsg': msg->msg_iov = (struct iovec *)vec, msg->msg_iovlen = num;
  // - kernel_recvmsg returns 'int'
  // - tmp == original 'count', count == 0
  //
  msg.msg_name        = NULL;
  msg.msg_namelen     = 0;
//  msg.msg_iov         = iov;
//  msg.msg_iovlen      = 0;
  msg.msg_control     = NULL;
  msg.msg_controllen  = 0;
  msg.msg_flags       = MSG_KERNSPACE;

  err = kernel_recvmsg( socket, &msg, iov, idx, tmp, MSG_WAITALL | MSG_NOCATCHSIG );

  socket->sk->sk_rcvtimeo = rcvtimeo;

  if ( likely( err == tmp ) ) {
    count = tmp;
    err   = 0;
  } else if ( err >= 0 ) {
    // We have read some data from socket (0?)
    count = err;
    err   = 0;
  } else {
    count = 0;
    err   = -EPIPE;
  }

  // Fix 'pos' based on returned bytes from recvmsg
  // Should we update pos if error?
  pos = start_pos + count;

  // Update valid size
  write_lock_irqsave( &u->rwlock, flags );
  if ( pos > u->valid ) {
    u->valid = pos;
    dirty    = 1;
  }
  write_unlock_irqrestore( &u->rwlock, flags );

  if ( unlikely( copy_to_user( off, &pos, sizeof(*off) ) ) )
    err = -EFAULT;

unmap_all:
  npages = idx; // save the number of pages
  while( 0 != idx-- ) {
    page = pages[idx];
    assert( !page_has_buffers( page ) );
    set_page_dirty( page );
    kunmap( page );
    flush_dcache_page( page );
    unlock_page( page );
    put_page( page );
  }

#ifdef Try_to_writeback_inodes_sb
  if ( unlikely( sbi->options.wbMb_in_pages ) )
    atomic_add( npages, &sbi->dirty_pages_count );
#endif

  if ( likely( 0 != count ) )
    balance_dirty_pages_ratelimited_nr( mapping, npages );

done:
  if ( dirty )
    mark_inode_dirty( i );

  current->backing_dev_info = NULL;
  Inode_unlock( i );

  kfree( iov );
  kfree( pages );

#ifdef Try_to_writeback_inodes_sb
  if ( unlikely( sbi->options.wb && count >= PAGE_SIZE ) ) {
    if ( atomic_dec_and_test( &sbi->writeiter_cnt ) ) {
      Try_to_writeback_inodes_sb( i->i_sb );
      atomic_set( &sbi->writeiter_cnt, sbi->options.wb );
    }
  } else if ( unlikely ( sbi->options.wbMb_in_pages ) ) {
    // Check if the number of pages exceeds the limit
    unsigned dirty_pages_count = atomic_read( &sbi->dirty_pages_count );
    if ( dirty_pages_count >= sbi->options.wbMb_in_pages ) {
      // Need this debug print for test_wbxm_option
      DebugTrace( 0, Dbg, ( "Call to Try_to_writeback_inodes_sb (%x)", dirty_pages_count ) );
      Try_to_writeback_inodes_sb( i->i_sb );
    }
  }
#endif

  if ( unlikely( 0 != err ) ) {
    DebugTrace( -1, Dbg, ("splice_write_from_socket failed -> %d", err ));
    return err;
  }

  DebugTrace( -1, Dbg, ("splice_write_from_socket -> %zx", count ));
  return count;
}
#endif // #if is_struct( FILE_OPERATIONS_SPLICE_WRITE_FROM_SOCKET ) && defined CONFIG_BCM_RECVFILE


///////////////////////////////////////////////////////////
// ufsd_file_sendpage
//
// file_operations::sendpage
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_sendpage(
    IN struct file  *file,
    IN struct page  *page,
    IN int          offset,
    IN size_t       len,
    IN OUT loff_t   *pos,
    IN int          more
    )
{
  return -EOPNOTSUPP;
}


static const struct file_operations ufsd_file_ops = {
  .llseek   = generic_file_llseek,
#ifdef UFSD_USE_AIO_READ_WRITE
  // 3.15-
  .read       = do_sync_read,
  .write      = do_sync_write, // need for loop
  .aio_read   = ufsd_file_aio_read,
  .aio_write  = ufsd_file_aio_write,
#else
  // 3.16+
#if is_decl( NEW_SYNC_READ )
  .read       = new_sync_read,
#endif
#if is_decl( NEW_SYNC_WRITE )
  .write      = new_sync_write, // need for loop
#endif
  .read_iter  = ufsd_file_read_iter,
  .write_iter = ufsd_file_write_iter,
#endif
#ifndef UFSD_NO_USE_IOCTL
  .unlocked_ioctl = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = ufsd_compat_ioctl,
#endif
#endif
  .mmap     = ufsd_file_mmap,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
  .fsync    = ufsd_file_fsync,
//  int (*aio_fsync) (struct kiocb *, int datasync);
//  int (*fasync) (int, struct file *, int);
  .sendpage = ufsd_file_sendpage,
#if is_decl( GENERIC_FILE_SPLICE_WRITE )
  .splice_write = ufsd_file_splice_write,
#endif
  .splice_read  = ufsd_file_splice_read,
#if is_struct( FILE_OPERATIONS_FALLOCATE )
  .fallocate    = ufsd_fallocate,
#endif
#if is_struct( FILE_OPERATIONS_SPLICE_WRITE_FROM_SOCKET ) && defined CONFIG_BCM_RECVFILE
  .splice_write_from_socket  = ufsd_splice_write_from_socket,
#endif
};


#ifdef UFSD_USE_HFS_FORK
// hfs+: Use special variant of 'inode_operations' to operate with forks
static const struct inode_operations ufsd_file_hfs_inode_ops = {
  .lookup       = ufsd_file_lookup,
  .create       = ufsd_file_create,
  .setattr      = ufsd_setattr,
//  .getattr      = ufsd_getattr,
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
  .permission   = ufsd_permission,
#if is_struct( INODE_OPERATIONS_GET_ACL )
  .get_acl      = ufsd_get_acl,
#endif
#if is_struct( INODE_OPERATIONS_SET_ACL )
  .set_acl      = ufsd_set_acl,
#endif
#endif
#endif
#if is_struct( INODE_OPERATIONS_FALLOCATE )
  .fallocate    = ufsd_fallocate,
#endif
};
#endif


#if !defined UFSD_HFS_ONLY || !defined UFSD_USE_HFS_FORK
// for ntfs/exfat/refs or hfs on 3.13+
static const struct inode_operations ufsd_file_inode_ops = {
  .setattr      = ufsd_setattr,
  .getattr      = ufsd_getattr,
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#ifdef CONFIG_FS_POSIX_ACL
  .permission   = ufsd_permission,
#if is_struct( INODE_OPERATIONS_GET_ACL )
  .get_acl      = ufsd_get_acl,
#endif
#if is_struct( INODE_OPERATIONS_SET_ACL )
  .set_acl      = ufsd_set_acl,
#endif
#endif
#endif
#if is_struct( INODE_OPERATIONS_FALLOCATE )
  .fallocate    = ufsd_fallocate,
#endif
};
#endif


#if defined UFSD_NTFS || defined UFSD_EXFAT || defined UFSD_REFS || defined UFSD_REFS3 || defined UFSD_FAT
///////////////////////////////////////////////////////////
// ufsd_readlink_hlp
//
// helper for  ufsd_readlink and ufsd_follow_link
///////////////////////////////////////////////////////////
static int
ufsd_readlink_hlp(
    IN usuper       *sbi,
    IN struct inode *i,
    OUT char        *kaddr,
    IN int          buflen
    )
{
  int len;
  char *p   = kaddr;
  char *l   = kaddr + buflen;
  unode *u  = UFSD_U(i);

  //
  // Call library code to read link
  //
  lock_ufsd( sbi );

  len = ufsdapi_read_link( u->ufile, kaddr, buflen );

  unlock_ufsd( sbi );

  if ( 0 != len )
    return -EFAULT;

  // safe strlen
  while( 0 != *p && p <= l )
    p += 1;
  len = (int)(p-kaddr);

#if defined UFSD_NTFS && !is_struct( FILE_SYSTEM_TYPE_MOUNT )
  //
  // Assume that link points to the same volume
  // and convert strings
  // C:\\Users => /mnt/ntfs/Users
  //
  if ( len > 3
    && 'a' <= (kaddr[0] | 0x20)
    && (kaddr[0] | 0x20) <= 'z'
    && ':' == kaddr[1]
    && '\\' == kaddr[2]
    && NULL != sbi->vfs_mnt ) {

#if is_decl( D_PATH_V1 )
    char * MntPath    = d_path( sbi->vfs_mnt->mnt_root, sbi->vfs_mnt, sbi->mnt_buffer, sizeof(sbi->mnt_buffer) - 1 );
#elif is_decl( D_PATH_V2 )
    struct path path  = { sbi->vfs_mnt, sbi->vfs_mnt->mnt_root };
    char * MntPath    = d_path( &path, sbi->mnt_buffer, sizeof(sbi->mnt_buffer) - 1 );
#endif

    if ( !IS_ERR( MntPath ) ) {
//      DebugTrace( 0, Dbg,("mount path %s", MntPath ));
      // Add last slash
      int MntPathLen = strlen( MntPath );
      MntPath[MntPathLen++] = '/';

      if ( MntPathLen + len - 3 < buflen ) {
        p = kaddr + MntPathLen;
        memmove( p, kaddr + 3, len - 3 );
        memcpy( kaddr, MntPath, MntPathLen );
        len += MntPathLen - 3;
        // Convert slashes
        l = kaddr + len;
        while( ++p < l ) {
          if ( '\\' == *p )
            *p = '/';
        }
        *p = 0;
      }
    }
  }
#endif

  return len;
}


///////////////////////////////////////////////////////////
// ufsd_readlink
//
// inode_operations::readlink
///////////////////////////////////////////////////////////
static int
ufsd_readlink(
    IN struct dentry  *de,
    OUT char __user   *buffer,
    IN int            buflen
    )
{
  int err;
  char *kaddr;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  VfsTrace( +1, Dbg, ("readlink: r=%lx, '%.*s', %d", i->i_ino, (int)de->d_name.len, de->d_name.name, buflen ));

  kaddr = ufsd_heap_alloc( buflen, 0 );
  if ( NULL == kaddr ) {
    err = -ENOMEM;
    goto out;
  }

  //
  // Call helper function that reads symlink into buffer
  //
  err = ufsd_readlink_hlp( sbi, i, kaddr, buflen );

  if ( err <= 0 || 0 != copy_to_user( buffer, kaddr, err ) )
    err = -EFAULT;

  ufsd_heap_free( kaddr );

out:
  VfsTrace( -1, Dbg, ("readlink -> %d", err ));

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_follow_link
//
// inode_operations::follow_link
///////////////////////////////////////////////////////////
#if is_decl( FOLLOW_LINK_V1 )
static void*
ufsd_follow_link(
    IN struct dentry    *de,
    IN struct nameidata *nd
    )
{
  void *ret;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  VfsTrace( +1, Dbg, ("follow_link: r=%lx, '%.*s'", i->i_ino, (int)de->d_name.len, de->d_name.name ));

  ret = kmalloc( PAGE_SIZE, GFP_NOFS );
  //
  // Call helper function that reads symlink into buffer
  //
  if ( NULL != ret && ufsd_readlink_hlp( sbi, i, ret, PAGE_SIZE ) > 0 )
    nd_set_link( nd, (char*)ret );

  VfsTrace( -1, Dbg, ("follow_link -> %p", ret ));

  return ret;
}
#elif is_decl( FOLLOW_LINK_V2 )
// This is version for 4.2+ kernels.
// Store opaque pointer in op and return symlink body or
// ERR_PTR on error or NULL on jump (procfs magic symlinks).
// Stored pointer is ignored in all cases except the last one.
static const char*
ufsd_follow_link(
    IN struct dentry    *de,
    OUT void            **op
    )
{
  void *ret;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  VfsTrace( +1, Dbg, ("follow_link: r=%lx, '%.*s'", i->i_ino, (int)de->d_name.len, de->d_name.name ));

  ret = kmalloc( PAGE_SIZE, GFP_NOFS );
  //
  // Call helper function that reads symlink into buffer
  //
  if ( NULL != ret ) {
    int err = ufsd_readlink_hlp( sbi, i, ret, PAGE_SIZE );
    if ( err > 0 ) {
      *op = ret;
    } else {
      kfree( ret );
      *op = ERR_PTR( err );
      ret = ERR_PTR( err );
    }
  } else {
    ret = ERR_PTR( -ENOMEM );
  }

  VfsTrace( -1, Dbg, ("follow_link -> %p", ret ));

  return (char*)ret;
}
#elif is_struct( INODE_OPERATIONS_GET_LINK )
static const char*
ufsd_get_link(
    IN struct dentry       *de,
    IN struct inode        *i,
    IN struct delayed_call *done
    )
{
  void *ret;
  usuper *sbi     = UFSD_SB( i->i_sb );

  if ( NULL == de ) {
    return ERR_PTR( -ECHILD );
  }

  VfsTrace( +1, Dbg, ("get_link: r=%lx, '%.*s'", i->i_ino, (int)de->d_name.len, de->d_name.name ));

  ret = kmalloc( PAGE_SIZE, GFP_NOFS );
  //
  // Call helper function that reads symlink into buffer
  //
  if ( NULL != ret ) {
    int err = ufsd_readlink_hlp( sbi, i, ret, PAGE_SIZE );
    if ( err > 0 ) {
      set_delayed_call( done, kfree_link, ret );
    } else {
      kfree( ret );
      ret = ERR_PTR( err );
    }
  } else {
    ret = ERR_PTR( -ENOMEM );
  }

  VfsTrace( -1, Dbg, ("get_link -> %p", ret ));

  return (char*)ret;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_put_link
//
// inode_operations::put_link
///////////////////////////////////////////////////////////
#if is_struct( INODE_OPERATIONS_PUT_LINK )
static void
ufsd_put_link(
#if is_decl( PUT_LINK_V1 )
    IN struct dentry    *de,
    IN struct nameidata *nd
#elif is_decl( PUT_LINK_V2 )
    IN struct inode     *i
#endif
    , IN void           *cookie
    )
{
  kfree( cookie );
}
#endif

static const struct inode_operations ufsd_link_inode_operations_ufsd = {
  .readlink    = ufsd_readlink,
#if is_struct( INODE_OPERATIONS_FOLLOW_LINK )
  .follow_link = ufsd_follow_link,
#elif is_struct( INODE_OPERATIONS_GET_LINK )
  .get_link = ufsd_get_link,
#endif
#if is_struct( INODE_OPERATIONS_PUT_LINK )
  .put_link    = ufsd_put_link,
#endif
  .setattr      = ufsd_setattr,
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#endif
};
#endif // #if defined UFSD_NTFS || defined UFSD_EXFAT || defined UFSD_REFS || defined UFSD_REFS3 || defined UFSD_FAT

#ifdef UFSD_HFS
static const struct inode_operations ufsd_link_inode_operations_u8 = {
#ifdef WRITE_SYNC
  .readlink     = generic_readlink,
#endif
#if is_struct( INODE_OPERATIONS_FOLLOW_LINK )
  .follow_link  = page_follow_link_light,
#elif is_struct( INODE_OPERATIONS_GET_LINK )
  .get_link     = page_get_link,
#endif
#if is_struct( INODE_OPERATIONS_PUT_LINK )
  .put_link     = page_put_link,
#endif
  .setattr      = ufsd_setattr,
#if is_decl( INODE_OPS_XATTR_ANY )
  .setxattr     = ufsd_setxattr,
  .getxattr     = ufsd_getxattr,
  .removexattr  = ufsd_removexattr,
#endif
#if defined UFSD_NTFS || defined UFSD_HFS
  .listxattr    = ufsd_listxattr,
#endif
};
#endif


typedef struct upage_data {
  loff_t      next_lbo;
  struct bio* bio;
} upage_data;


///////////////////////////////////////////////////////////
// ufsd_end_buffer_async_read
//
// this function comes from fs/buffer.c 'end_buffer_async_read'
///////////////////////////////////////////////////////////
static void
ufsd_end_buffer_async_read(
    IN struct buffer_head* bh,
    IN int uptodate
    )
{
  unsigned long flags;
  struct buffer_head  *tmp;
  int page_uptodate = 1;
  struct page* page = bh->b_page;
  struct buffer_head  *first = page_buffers( page );
  struct inode* i   = page->mapping->host;

  if ( likely( uptodate ) ) {
#if 0
    set_buffer_uptodate( bh );
#else
    unode *u           = UFSD_U( i );
    unsigned bh_off    = bh_offset( bh );
    loff_t buffer_off  = bh_off + ((loff_t)page->index << PAGE_SHIFT);
    loff_t i_size, valid = get_valid_size( u, &i_size, NULL );
    if ( valid > i_size )
      valid = i_size;

    set_buffer_uptodate( bh );

    if ( buffer_off + bh->b_size > valid ) {
      unsigned off = valid > buffer_off? valid - buffer_off : 0;

      local_irq_save( flags );
      zero_user_segment( page, bh_off + off, bh_off + bh->b_size );
      local_irq_restore( flags );
    }
#endif
  } else {
    clear_buffer_uptodate( bh );
    SetPageError( page );
    printk_ratelimited( KERN_ERR QUOTED_UFSD_DEVICE ": \"%s\" (\"%s\"): buffer read error, logical block 0x%" PSCT "x.", current->comm, i->i_sb->s_id, bh->b_blocknr );
  }

  local_irq_save( flags );
  bit_spin_lock( BH_Uptodate_Lock, &first->b_state );
  clear_buffer_async_read( bh );
  unlock_buffer( bh );
  tmp = bh;
  do {
    if ( !buffer_uptodate( tmp ) ) {
      page_uptodate = 0;
    }
    if ( buffer_async_read( tmp ) ) {
      BUG_ON(!buffer_locked( tmp ));
      bit_spin_unlock( BH_Uptodate_Lock, &first->b_state );
      local_irq_restore( flags );
      return;
    }
  } while( bh != ( tmp = tmp->b_this_page ) );

  bit_spin_unlock( BH_Uptodate_Lock, &first->b_state );
  local_irq_restore( flags );

  if ( page_uptodate && !PageError( page ) )
    SetPageUptodate( page );
  unlock_page( page );
}


///////////////////////////////////////////////////////////
// ufsd_end_io_read
//
// I/O completion handler for multipage BIOs
///////////////////////////////////////////////////////////
static void
ufsd_end_io_read(
    IN struct bio *bio
#ifdef BIO_UPTODATE
    , IN int        error
#endif
    )
{
  struct bio_vec *bvec = &bio->bi_io_vec[bio->bi_vcnt-1];
  struct inode *i = bvec->bv_page->mapping->host;
  unode *u        = UFSD_U( i );
  int err         = BIO_RESULT( bio );

//  printk( "end_io_read at %llx sz=%x, cnt=%x\n", (UINT64)BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt );

  if ( err )
    printk_ratelimited( KERN_ERR QUOTED_UFSD_DEVICE ": \"%s\" (\"%s\"): bio read error", current->comm, NULL == i? "" : i->i_sb->s_id );

  do {
    struct page *page = bvec->bv_page;
    if ( !err ) {
      unsigned long flags;
      loff_t page_off = (loff_t)page->index << PAGE_SHIFT;
      loff_t i_size, valid = get_valid_size( u, &i_size, NULL );
      if ( valid > i_size )
        valid = i_size;

      if ( page_off + PAGE_SIZE > valid ) {
        local_irq_save( flags );
        zero_user_segment( page, valid > page_off? valid - page_off : 0, PAGE_SIZE );
        local_irq_restore( flags );
      }
      SetPageUptodate( page );
    } else {
      ClearPageDirty( page );
      SetPageError( page );
    }
    unlock_page( page );
  } while ( --bvec >= bio->bi_io_vec );
  bio_put( bio );
}


///////////////////////////////////////////////////////////
// ufsd_end_io_write
//
// I/O completion handler for multipage BIOs
///////////////////////////////////////////////////////////
static void
ufsd_end_io_write(
    IN struct bio *bio
#ifdef BIO_UPTODATE
    , IN int        error
#endif
    )
{
  struct bio_vec *bvec = &bio->bi_io_vec[bio->bi_vcnt-1];
  int err = BIO_RESULT( bio );

  if ( err ) {
    struct inode* i = bio->bi_io_vec[0].bv_page->mapping->host;
    printk_ratelimited( KERN_ERR QUOTED_UFSD_DEVICE ": \"%s\" (\"%s\"): bio write error", current->comm, NULL == i? "" : i->i_sb->s_id );
  }

//  printk( "end_io_write at %llx sz=%x, cnt=%x\n", (UINT64)BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt );

  do {
    struct page* page = bvec->bv_page;
    if ( err ) {
      SetPageError( page );
      set_bit( AS_EIO, &page->mapping->flags );
    }

    end_page_writeback( page );
  } while( --bvec >= bio->bi_io_vec );

  bio_put( bio );
}


///////////////////////////////////////////////////////////
// mpage_alloc
//
// allocates bio for 'nr_vecs' of iovecs
///////////////////////////////////////////////////////////
static struct bio*
mpage_alloc(
    IN struct block_device *bdev,
    IN sector_t first_sector,
    IN unsigned nr_vecs
    )
{
  while( 1 ) {
    struct bio *bio = bio_alloc( GFP_NOFS|__GFP_HIGH, nr_vecs ); // GFP_NOIO
    if ( likely( NULL != bio ) ) {
      BIO_BISECTOR( bio ) = first_sector;
      bio->bi_bdev    = bdev;

      DebugTrace( 0, UFSD_LEVEL_BIO, ("bio+: o=%" PSCT "x", first_sector << 9 ));
      return bio;
    }

    if ( !(current->flags & PF_MEMALLOC) )
      return NULL;

    nr_vecs >>= 1;
    if ( 0 == nr_vecs )
      return NULL;
  }
}


///////////////////////////////////////////////////////////
// ufsd_bio_read_submit
//
// submit read bio ( from fs/mpage.c )
///////////////////////////////////////////////////////////
static void
ufsd_bio_read_submit(
    IN struct bio *bio
    )
{
  assert( 0 == (BIO_BISIZE( bio ) & 0x1ff) );
  DebugTrace( 0, UFSD_LEVEL_BIO, ("submit_bio read at o=%" PSCT "x, sz=%x, cnt=%x", BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt ));
  bio->bi_end_io = ufsd_end_io_read;
  Submit_bio( READ, 0, bio );
}


///////////////////////////////////////////////////////////
// ufsd_bio_write_submit
//
// we count pages, that are about to be written to disk and
// submit write bio ( from fs/mpage.c )
///////////////////////////////////////////////////////////
static void
ufsd_bio_write_submit(
    IN struct bio *bio,
    IN OUT usuper *sbi
    )
{
#ifdef Try_to_writeback_inodes_sb
  if ( unlikely( sbi->options.wbMb_in_pages )
    && atomic_sub_return( bio->bi_vcnt, &sbi->dirty_pages_count ) < 0 ) {
#ifdef UFSD_DEBUG
    ufsd_printk( sbi->sb, " dirty_pages_count < 0 " );
#endif
    atomic_set( &sbi->dirty_pages_count, 0 );
  }
#endif

  assert( 0 == (BIO_BISIZE( bio ) & 0x1ff) );
  DebugTrace( 0, UFSD_LEVEL_BIO, ("submit_bio write at o=%" PSCT "x, sz=%x, cnt=%x", BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt ));
  bio->bi_end_io = ufsd_end_io_write;
  Submit_bio( WRITE, 0, bio );
}


#ifdef UFSD_NTFS
///////////////////////////////////////////////////////////
// ufsd_read_ntfs_file
//
// Helper function to read resident/compressed files
///////////////////////////////////////////////////////////
static int
ufsd_read_ntfs_file(
    IN usuper *sbi,
    IN unode  *u,
    IN struct page *page,
    IN loff_t vbo
    )
{
  size_t ret;
  int err;
  char* kaddr;
  unsigned from = vbo & ~PAGE_MASK;

  //
  // Read file via UFSD -> ufsd_bd_read
  //
  DebugTrace( 0, Dbg, ("r=%lx: use ufsd to read at off %llx", u->i.i_ino, vbo ));

  lock_ufsd( sbi );

  kaddr = kmap( page );
  err   = ufsdapi_file_read( sbi->ufsd, u->ufile, NULL, 0, vbo - from, PAGE_SIZE, kaddr, &ret );
  if ( likely( 0 == err ) ) {
    if ( ret < PAGE_SIZE )
      memset( kaddr + ret, 0, PAGE_SIZE - ret );
    SetPageUptodate( page );
  } else {
    ret = err;
    SetPageError( page );
  }
  kunmap( page );
  flush_dcache_page( page );

  unlock_ufsd( sbi );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_write_ntfs_file
//
// Helper function to write resident/compressed files
///////////////////////////////////////////////////////////
static int
ufsd_write_ntfs_file(
    IN usuper *sbi,
    IN unode  *u,
    IN struct page *page,
    IN loff_t vbo,
    IN size_t len
    )
{
  int err;
  loff_t i_size;

  //
  // Write file via UFSD -> ufsd_bd_write
  //
  DebugTrace( 0, Dbg, ("r=%lx: use ufsd to write at off %llx + %zx", u->i.i_ino, vbo, len ));

  lock_ufsd( sbi );

  i_size = i_size_read( &u->i );
  if ( vbo <= i_size ) {
    size_t written;
    char* kaddr;
    unsigned from = vbo & ~PAGE_MASK;
    if ( vbo + len > i_size )
      len = i_size - vbo;

    kaddr = kmap( page ); // atomic_kmap( page ); for resident?
    err   = ufsdapi_file_write( sbi->ufsd, u->ufile, NULL, 0, vbo, len, kaddr + from, &written );
    kunmap( page );

  } else {
    err = 0;
  }

  unlock_ufsd( sbi );

  if ( unlikely( 0 != err ) ) {
    ufsd_printk( u->i.i_sb, "failed to write inode 0x%lx, %d", u->i.i_ino, err );
    return -EIO;
  }

  return 0;
}
#endif // #ifdef UFSD_NTFS


///////////////////////////////////////////////////////////
// ufsd_buf_readpage
//
// fs/buffer.c 'block_read_full_page'
///////////////////////////////////////////////////////////
static int
ufsd_buf_readpage(
    IN usuper       *sbi,
    IN unode        *u,
    IN struct page  *page,
    IN loff_t       page_off,
    IN mapinfo      *map
    )
{
  struct inode *i = &u->i;
  loff_t i_size   = i_size_read( i );
  int err         = 0;

  assert( page_off == ((loff_t)page->index << PAGE_SHIFT) );
  assert( !is_compressed( u ) );

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("buf_readpage: r=%lx, %llx, sz=%llx,%llx", i->i_ino, page_off, u->valid, i_size ));

  ProfileEnter( sbi, buf_readpage );

  BUG_ON( !PageLocked( page ) );

#ifndef UFSD_USE_BH
  if ( unlikely( page->index >= ( i_size + PAGE_SIZE - 1 ) >> PAGE_SHIFT ) ) {
//Zero:
    zero_user_segment( page, 0, PAGE_SIZE );
    SetPageUptodate( page );
    unlock_page( page );
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage (zero) -> ok" ));
  } else
#endif
  {
    unsigned blkbits   = i->i_blkbits;
    unsigned blocksize = 1 << blkbits;
    struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
    int nr;
//    int fully_mapped = 1;
    loff_t valid, vbo  = page_off;

    BUG_ON( PageUptodate( page ) );

    if ( !page_has_buffers( page ) ) {
      create_empty_buffers( page, blocksize, 0 );
      if ( !page_has_buffers( page ) ) {
        unlock_page( page );
        ProfileLeave( sbi, buf_readpage );
        DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage -> nomemory" ));
        return -ENOMEM;
      }
    }

    head  = page_buffers( page );
    valid = get_valid_size( u, NULL, NULL );
    if ( valid > i_size )
      valid = i_size;

    bh  = head;
    nr  = 0;

    do {

      if ( buffer_uptodate( bh ) )
        goto next_block;

      if ( buffer_mapped( bh ) ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage - already mapped %llx, bh=%" PSCT "x", vbo, bh->b_blocknr ));
      } else {
//        fully_mapped = 0;

        if ( vbo >= i_size ) {
zero_buf:
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage - zero bh page: [%lx + %x)", bh_offset( bh ), blocksize ));
          zero_user_segment( page, bh_offset( bh ), bh_offset( bh ) + blocksize );
          set_buffer_uptodate( bh );
          goto next_block;
        }

        //
        // map to read
        //
        if ( map->len < blocksize && unlikely( vbo_to_lbo( sbi, u, vbo, 0, map ) ) ) {
          DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** buf_readpage - failed to get map for r=%lx, %llx, %llx, %llx", i->i_ino, vbo, valid, i_size ));
          SetPageError( page );
          goto zero_buf;
        }

        if ( 0 == map->len )
          goto zero_buf;

#ifdef UFSD_NTFS
        if ( is_ntfs( &sbi->options ) ) {
          if ( UFSD_VBO_LBO_HOLE == map->lbo )
            goto zero_buf;

          if ( UFSD_VBO_LBO_RESIDENT == map->lbo || UFSD_VBO_LBO_COMPRESSED == map->lbo ) {
            assert( 0 == nr );

            err = ufsd_read_ntfs_file( sbi, u, page, vbo );
            if ( err < 0 )
              break;

            do {
//              assert( !buffer_dirty( bh ) );
//              clear_buffer_dirty( bh );
              set_buffer_uptodate( bh );
            } while ( (bh = bh->b_this_page) != head );

            SetPageUptodate( page );

            if ( page_off + PAGE_SIZE > i_size ) {
              unsigned long flags;
              local_irq_save( flags );
              zero_user_segment( page, i_size > page_off? i_size - page_off : 0, PAGE_SIZE );
              local_irq_restore( flags );
            }
  //          set_buffer_uptodate( bh );
            //continue;
            break;
          }
        }
#endif // #ifdef UFSD_NTFS

        bh->b_bdev    = i->i_sb->s_bdev;
        bh->b_blocknr = map->lbo >> blkbits;
        set_buffer_mapped( bh );
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage - set_mapped %llx => b=%" PSCT "x", vbo, bh->b_blocknr ));

        if ( vbo >= valid ) {
//zero_buf:
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage - zero bh page: [%lx + %x)", bh_offset( bh ), blocksize ));
          zero_user_segment( page, bh_offset( bh ), bh_offset( bh ) + blocksize );
          set_buffer_uptodate( bh );
          goto next_block;
        }
      }

      arr[nr++] = bh;

next_block:
      if ( map->len < blocksize )
        map->len = 0;
      else {
        map->len -= blocksize;
        if ( is_lbo_ok( map->lbo ) )
          map->lbo += blocksize;
      }
      vbo += blocksize;

    } while( (bh = bh->b_this_page) != head );

//    if ( fully_mapped )
//      SetPageMappedToDisk( page );

    if ( !nr ) {
      if ( !PageError( page ) )
        SetPageUptodate( page );
      unlock_page( page );
    } else {

      int k;

      for ( k = 0; k < nr; ++k ) {
        bh = arr[k];
        lock_buffer( bh );
        bh->b_end_io = ufsd_end_buffer_async_read;
        set_buffer_async_read( bh );
      }

      for ( k = 0; k < nr; ++k ) {
        bh = arr[k];
        if ( buffer_uptodate( bh ) ) {
          ufsd_end_buffer_async_read( bh, 1 );
        } else {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("submit_bh( b=%" PSCT "x, r )", bh->b_blocknr));
          Submit_bh( READ, 0, bh );
        }
      }
    }
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage -> %d, nr=%d", nr, err ));
  }

  ProfileLeave( sbi, buf_readpage );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_do_readpage
//
// based on fs/mpage.c 'do_mpage_readpage'
///////////////////////////////////////////////////////////
static int
ufsd_do_readpage(
    IN struct page    *page,
    IN unsigned       nr_pages,
    IN OUT upage_data *mpage
    )
{
  int err;
  struct inode *i = page->mapping->host;
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  loff_t page_off = (loff_t)page->index << PAGE_SHIFT;
  mapinfo map;

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("do_readpage: r=%lx, o=%llx, sz=%llx,%llx", i->i_ino, page_off, u->valid, i->i_size ));

  ProfileEnter( sbi, do_readpage );

#ifdef UFSD_USE_BH
  map.len = 0;
#else
  if ( !page_has_buffers( page ) ) {
    //
    // Check if we can read page without buffers
    //
    unsigned bh_off;
    loff_t start_lbo, valid;

    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_readpage - no buf at page" ));

#ifdef UFSD_NTFS
    if ( is_compressed( u ) ) {
UseUfsd:
      err = ufsd_read_ntfs_file( sbi, u, page, page_off );
      unlock_page( page );
      if ( err > 0 )
        err = 0;

      ProfileLeave( sbi, do_readpage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> %d", err ));
      return err;
    }
#endif

    valid = get_valid_size( u, NULL, NULL );

    //
    // map to read
    //
    if ( likely( page_off < valid && 0 == vbo_to_lbo( sbi, u, page_off, 0, &map ) ) ) {
#ifdef UFSD_NTFS
      if ( is_ntfs( &sbi->options ) ) {
        if ( UFSD_VBO_LBO_HOLE == map.lbo ) {
          if ( map.len >= PAGE_SIZE )
            goto zero;
        } else if ( UFSD_VBO_LBO_RESIDENT == map.lbo )
          goto UseUfsd;
        else if ( UFSD_VBO_LBO_COMPRESSED == map.lbo ) {
          assert( !"Impossible" );
          goto UseUfsd;
        }
      }
#endif // #ifdef UFSD_NTFS

      //
      // Check page for continues
      //
      if ( map.len < PAGE_SIZE ) {
#ifdef UFSD_NTFS
        if ( UFSD_VBO_LBO_HOLE == map.lbo && page_off + map.len >= valid ) {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_readpage - zero page" ));
          goto zero;
        }
#endif
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_readpage confused(1)" ));
        goto confused;
      }

      if ( page_off + PAGE_SIZE > valid ) {
//        bh_off  = valid - page_off;
        zero_user_segment( page, valid - page_off, PAGE_SIZE );
      } else {
//        bh_off  = PAGE_CACHE_SIZE;
      }
      bh_off  = PAGE_SIZE;
      start_lbo = map.lbo;
    } else {
#ifdef UFSD_NTFS
zero:
#endif
      zero_user_segment( page, 0, PAGE_SIZE );
      SetPageUptodate( page );
      unlock_page( page );
      ProfileLeave( sbi, do_readpage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> full zero" ));
      return 0;
    }

    //
    // here we have valid 'start_lbo'
    // Try to merge with previous request
    //
    if ( NULL != mpage->bio && mpage->next_lbo != start_lbo ) {
      ufsd_bio_read_submit( mpage->bio );
      goto alloc_new;
    }

    if ( NULL == mpage->bio ) {
      struct block_device *bdev;
alloc_new:
      bdev = i->i_sb->s_bdev;
      if ( start_lbo >= sbi->dev_size ) {
        ufsd_printk( i->i_sb, "do_readpage: r=%lx, o=%llx, sz=%llx,%llx: start_lbo %llx >= dev_size %llx",
                     i->i_ino, page_off, u->valid, i->i_size, start_lbo, sbi->dev_size );
        BUG_ON( 1 );
      }
      mpage->bio = mpage_alloc( bdev, start_lbo >> 9,
                                min_t( unsigned, bio_get_nr_vecs( bdev ), min_t( unsigned, BIO_MAX_PAGES, nr_pages ) ) );
      if ( NULL == mpage->bio )
        goto buf_read;
    }

    //
    // Read head (full) page
    //
    assert( 0 != bh_off );
    if ( bio_add_page( mpage->bio, page, bh_off, 0 ) < bh_off ) {
      ufsd_bio_read_submit( mpage->bio );
      goto alloc_new;
    }

    if ( PAGE_SIZE == bh_off ) {
      mpage->next_lbo = start_lbo + PAGE_SIZE;
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> ok, next=%llx", mpage->next_lbo ));
    } else {
      ufsd_bio_read_submit( mpage->bio );
      mpage->bio = NULL;
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> ok, submitted" ));
    }

    ProfileLeave( sbi, do_readpage );
    return 0;
  }

  map.len = 0;

confused:
  if ( NULL != mpage->bio ) {
    ufsd_bio_read_submit( mpage->bio );
    mpage->bio = NULL;
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_readpage, submitted" ));
  }

buf_read:
#endif // #ifndef UFSD_USE_BH

  err = ufsd_buf_readpage( sbi, u, page, page_off, &map );

  ProfileLeave( sbi, do_readpage );
  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> %d (buf)", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_readpage
//
// address_space_operations::readpage
///////////////////////////////////////////////////////////
static int
ufsd_readpage(
    IN struct file *file,
    IN struct page *page
    )
{
  int err;
  struct inode *i = page->mapping->host;
  upage_data mpage;
  DEBUG_ONLY( usuper *sbi = UFSD_SB( i->i_sb ); )

  mpage.bio = NULL;

  ProfileEnter( sbi, readpage );

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("readpage: r=%lx, o=%llx", i->i_ino, (UINT64)page->index << PAGE_SHIFT ));

  err = ufsd_do_readpage( page, 1, &mpage );
  if ( NULL != mpage.bio )
    ufsd_bio_read_submit( mpage.bio );

  ProfileLeave( sbi, readpage );

  if ( likely( 0 == err ) ) {
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("readpage -> ok%s", mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("readpage -> err %d%s", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "failed to read page 0x%llx for inode 0x%lx, (error %d)", (UINT64)page->index, i->i_ino, err );
  }
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_buf_writepage
//
// fs/buffer.c 'block_write_full_page'
///////////////////////////////////////////////////////////
static int
ufsd_buf_writepage(
    IN usuper       *sbi,
    IN unode        *u,
    IN struct page  *page,
    IN struct writeback_control *wbc,
    IN loff_t       page_off,
    IN mapinfo      *map
    )
{
  struct inode *i   = &u->i;
  loff_t i_size     = i_size_read( i );
  pgoff_t index     = page->index;
  pgoff_t end_index = i_size >> PAGE_SHIFT;
  struct super_block *sb = i->i_sb;
  unsigned blkbits    = i->i_blkbits;
  unsigned blocksize  = 1 << blkbits;
  struct buffer_head *bh, *head;
  loff_t vbo;
  int err = 0, all_done;

  assert( page_off == ((loff_t)index << PAGE_SHIFT) );
  assert( !is_compressed( u ) );

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("buf_writepage: o=%llx, sz=%llx,%llx", page_off, u->valid, i_size ));

  ProfileEnter( sbi, buf_writepage );

  BUG_ON( !PageLocked( page ) );

  if ( unlikely( index >= end_index ) ) {
    unsigned offset = i_size & (PAGE_SIZE-1);
    if ( unlikely( index >= end_index+1 || !offset ) ) {
#if is_decl( BLOCK_INVALIDATEPAGE_V1 )
      block_invalidatepage( page, 0 );
#elif is_decl( BLOCK_INVALIDATEPAGE_V2 )
      block_invalidatepage( page, 0, PAGE_SIZE );
#endif
      unlock_page( page );
      ProfileLeave( sbi, buf_writepage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage - (out of size) => 0" ));
      return 0;
    }

    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - zero_user_segment from %x", offset ));
    zero_user_segment( page, offset, PAGE_SIZE );
  }

  //
  // Below is modified variant of '__block_write_full_page'
  //
  if ( !page_has_buffers( page ) ) {
//    DebugTrace( 0, Dbg, ("buf_writepage: create page buffers" ));
    create_empty_buffers( page, blocksize, (1u<<BH_Uptodate) | (1u<<BH_Dirty) );

    if ( !page_has_buffers( page ) ) {
      printk( "Error allocating page buffers.  Redirtying page so we try again later." );
      redirty_page_for_writepage( wbc, page );
      unlock_page( page );
      ProfileLeave( sbi, buf_writepage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage - no memory" ));
      return 0;
    }
  }

  head  = page_buffers( page );
  bh    = head;
  vbo   = page_off;

  do {
    loff_t block_end = vbo + blocksize;
    size_t towrite;
    assert( page == bh->b_page );

    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - o=%llx b=%" PSCT "x, st=%lx", vbo, bh->b_blocknr, bh->b_state ));

    if ( vbo >= i_size ) {
      clear_buffer_dirty( bh );
      set_buffer_uptodate( bh );
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - o=%llx out of size", vbo ));
      goto next_block;
    }

    if ( buffer_mapped( bh ) ) {
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - buffer mapped o=%llx -> b=%" PSCT "x", vbo, bh->b_blocknr ));
      goto next_block;
    }

    if ( !buffer_dirty( bh ) ) {
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - not dirty %llx -> b=%" PSCT "x", vbo, bh->b_blocknr ));
      goto next_block;
    }

    bh->b_bdev  = sb->s_bdev;
    towrite     = block_end > i_size ? (size_t)(i_size - vbo) : blocksize;

    //
    // map to read (to write if sparsed)
    //
    if ( map->len < blocksize && unlikely( 0 != vbo_to_lbo( sbi, u, vbo, is_sparsed_or_compressed( u )? towrite : 0, map ) ) ) {
      ufsd_printk( i->i_sb, "failed to map r=%lx, vbo %llx", i->i_ino, vbo );

nomap:
//      bh->b_blocknr = -1;
      clear_buffer_mapped( bh );
      clear_buffer_dirty( bh );
      zero_user_segment( page, bh_offset( bh ), bh_offset( bh ) + blocksize );
      set_buffer_uptodate( bh );
      goto next_block;
    }

    if ( 0 == map->len )
      goto nomap;

#ifdef UFSD_NTFS
    if ( is_ntfs( &sbi->options ) ) {
      assert( UFSD_VBO_LBO_COMPRESSED != map->lbo );
      assert( UFSD_VBO_LBO_HOLE != map->lbo );
      if ( UFSD_VBO_LBO_HOLE == map->lbo || UFSD_VBO_LBO_COMPRESSED == map->lbo )
        goto nomap;

      if ( UFSD_VBO_LBO_RESIDENT == map->lbo ) {
        //
        // File is resident
        //
        err = ufsd_write_ntfs_file( sbi, u, page, vbo + bh_offset( bh ), towrite );
        if ( unlikely( 0 != err ) )
          break;

//      SetPageUptodate( page );
        set_buffer_uptodate( bh );
        clear_buffer_dirty( bh );
        goto next_block;
      }
    }
#endif

    bh->b_blocknr = map->lbo >> blkbits;
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - set_mapped %llx => b=%" PSCT "x", vbo, bh->b_blocknr ));
    set_buffer_mapped( bh );

next_block:
    if ( map->len < blocksize )
      map->len = 0;
    else {
      map->len -= blocksize;
      if ( is_lbo_ok( map->lbo ) )
        map->lbo += blocksize;
    }
    vbo = block_end;

  } while( (bh = bh->b_this_page) != head );

  if ( !PageUptodate( page ) ) {
    bh = head;
    while ( buffer_uptodate( bh ) ) {
      bh = bh->b_this_page;
      if ( head == bh ) {
        // All buffers uptodate -> page uptodate
        SetPageUptodate( page );
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage - SetPageUptodate" ));
        break;
      }
    }
  }

  bh = head;
  do {
    if ( !buffer_mapped( bh ) || !buffer_dirty( bh ) ) {
      if ( err && -ENOMEM != err )
        clear_buffer_dirty( bh );
      continue;
    }
    lock_buffer( bh );
    if ( test_clear_buffer_dirty( bh ) ) {
      mark_buffer_async_write( bh );
    } else{
      unlock_buffer( bh );
    }

  } while ( (bh = bh->b_this_page) != head );

  if ( unlikely( 0 != err ) )  {
    if ( -EOPNOTSUPP == err )
      err = 0;
    else if ( -ENOMEM == err ) {
      ufsd_printk( sb, "error allocating memory. redirtying page to try again later." );
      redirty_page_for_writepage( wbc, page );
      err = 0;
    } else{
      mapping_set_error( page->mapping, err );
      SetPageError( page );
    }
  }

  BUG_ON( PageWriteback( page ) );
  all_done = 1;
  set_page_writeback( page );

  assert( bh == head );
  do {
    struct buffer_head *next = bh->b_this_page;
    if ( buffer_async_write( bh ) ) {
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("submit_bh( b=%" PSCT "x, w )", bh->b_blocknr ));
      Submit_bh( WRITE, 0, bh );
      all_done = 0;
    }
    bh = next;
  } while( bh != head );

  unlock_page( page );
  if ( all_done )
    end_page_writeback( page );

  ProfileLeave( sbi, buf_writepage );
  // TODO: update 'arch' bit for exfat/ntfs
  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage => %d, sz=%llx,%llx, %s", err, u->valid, i_size, all_done? "done":"wb" ));

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_do_writepage
//
// based on fs/mpage.c '__mpage_writepage'
///////////////////////////////////////////////////////////
static int
ufsd_do_writepage(
    IN struct page *page,
    IN struct writeback_control *wbc,
    IN OUT upage_data *mpage
    )
{
  loff_t start_lbo    = 0; // not necessary, just to suppress warnings
  struct address_space *mapping = page->mapping;
  struct inode *i     = mapping->host;
  usuper *sbi         = UFSD_SB( i->i_sb );
  unode *u            = UFSD_U( i );
  unsigned blkbits    = i->i_blkbits;
  unsigned blocksize  = 1 << blkbits;
  struct buffer_head *head, *bh;
  unsigned first_unmapped = PAGE_SIZE;  // assume that no mapped buffers
//  int uptodate;
  unsigned bh_off = 0;
  int err;
  mapinfo map;
  loff_t i_size, valid = get_valid_size( u, &i_size, NULL );
  loff_t page_off = (loff_t)page->index << PAGE_SHIFT;

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("do_writepage(%s): r=%lx, o=%llx, sz=%llx,%llx", current->comm, i->i_ino, page_off, u->valid, i->i_size ));

  ProfileEnter( sbi, do_writepage );

  if ( page_off >= i_size || NULL == u->ufile ) {

//    ufsd_printk( i->i_sb, "do_writepage(%s): r=%lx, o=%llx, sz=%llx,%llx) -> out of file %p", current->comm, i->i_ino, page_off, u->valid, i->i_size, u->ufile );

    BUG_ON( PageWriteback( page ) );
    set_page_writeback( page );
    unlock_page( page );
    end_page_writeback( page );

    ProfileLeave( sbi, do_writepage );
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> out of file, sz=%llx,%llx", u->valid, i->i_size ));
    return 0;
  }

  map.len = 0;

  if ( unlikely( page_has_buffers( page ) ) ) {
    loff_t vbo = page_off;
    bh = head  = page_buffers( page );
//    uptodate   = 1;
    assert( !is_compressed( u ) );

    //
    // Check for all buffers in page
    //
    do {
      loff_t lbo;
      BUG_ON( buffer_locked( bh ) );
      if ( !buffer_mapped( bh ) ) {
        if ( buffer_dirty( bh ) ) {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(1) o=%llx v=%llx", vbo, valid ));
          goto confused;
        }
//        if ( !buffer_uptodate( bh ) )
//          uptodate = 0;
        // Save the position of hole
        if ( PAGE_SIZE == first_unmapped )
          first_unmapped = bh_off; // save the position of first unmapped buffer in page
        continue;
      }

      if ( first_unmapped != PAGE_SIZE ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(2) o=%llx v=%llx", vbo, valid ));
        goto confused;  // hole -> non-hole
      }

      if ( !buffer_dirty( bh ) || !buffer_uptodate( bh ) ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(3) o=%llx v=%llx", vbo, valid ));
        goto confused;
      }

      if ( i_size > valid && vbo >= valid ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(4) o=%llx v=%llx", vbo, valid ));
        goto confused;
      }

      lbo = (loff_t)bh->b_blocknr << blkbits;

      if ( lbo >= sbi->dev_size ) {
        ufsd_printk( i->i_sb, "do_writepage (bh): r=%lx, o=%llx, sz=%llx,%llx: bh=%" PSCT "x, lbo %llx >= dev_size %llx",
                     i->i_ino, page_off, u->valid, i->i_size, bh->b_blocknr, lbo, sbi->dev_size );
        BUG_ON( 1 );
      }

      if ( 0 == bh_off ) {
        start_lbo = lbo;
      } else if ( lbo == start_lbo + bh_off ) {
        // page still is continues
      } else {
        // page is not continues
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(5) o=%llx v=%llx", vbo, valid ));
        goto confused;
      }

      vbo     += blocksize;
      bh_off  += blocksize;

    } while ( head != ( bh = bh->b_this_page ) );

    if ( 0 == first_unmapped ) {
      // Page is full unmapped
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(6) o=%llx v=%llx", vbo, valid ));
      goto confused;
    }
    // First 'first_unmapped' is mapped

  } else {

    unsigned towrite = (page_off + PAGE_SIZE) > i_size? (i_size - page_off) : PAGE_SIZE;

#ifdef UFSD_NTFS
    if ( is_compressed( u ) ) {
UseUfsd:
      err = ufsd_write_ntfs_file( sbi, u, page, page_off, towrite );

      ClearPageDirty( page );
      if ( unlikely( 0 != err ) ) {
        SetPageError( page );
      } else {
        page_off += towrite;
        if ( likely( page_off >= valid ) )
          set_valid_size( u, page_off );
        SetPageUptodate( page );
      }

      BUG_ON( PageWriteback( page ) );
      set_page_writeback( page );
      unlock_page( page );
      end_page_writeback( page );

      ProfileLeave( sbi, do_writepage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> %d, sz=%llx,%llx", err, u->valid, i_size ));
      return err;
    }
#endif

    //
    // map to read (to write if sparsed)
    //
    if ( 0 != vbo_to_lbo( sbi, u, page_off, is_sparsed_or_compressed( u )? towrite : 0, &map ) ) {
      ufsd_printk( i->i_sb, "**** do_writepage (failed to map): r=%lx, o=%llx, sz=%llx,%llx: out of file",
                   i->i_ino, page_off, valid, i_size );
      goto confused;
    }

    if ( 0 == map.len ) {
//      ufsd_printk( i->i_sb, "**** do_writepage (out of file): r=%lx, o=%llx, sz=%llx,%llx: out of file",
//                   i->i_ino, page_off, valid, i_size );
      goto confused;
    }

#ifdef UFSD_NTFS
    if ( is_ntfs( &sbi->options ) ) {
      if ( UFSD_VBO_LBO_RESIDENT == map.lbo )
        goto UseUfsd;
      if ( UFSD_VBO_LBO_COMPRESSED == map.lbo ) {
        assert( !"Impossible" );
        goto UseUfsd;
      }
      BUG_ON( UFSD_VBO_LBO_HOLE == map.lbo );
    }
#endif // #ifdef UFSD_NTFS

    //
    // Check page for continues
    //
    if ( map.len < PAGE_SIZE )
      goto confused;

//    if ( vbo + PAGE_CACHE_SIZE > valid )
//      set_valid_size( u, vbo + PAGE_CACHE_SIZE );
    start_lbo = map.lbo;
    bh_off    = PAGE_SIZE;
  }

  //
  // 'blocks' is not empty. Check if we can merge with previous bio
  //
  if ( NULL != mpage->bio && mpage->next_lbo != start_lbo ) {
    //
    // Write previous fragment
    //
    ufsd_bio_write_submit( mpage->bio, sbi );
    goto alloc_new;
  }

  if ( NULL == mpage->bio ) {
    struct block_device *bdev;
alloc_new:
#if 0
    if (first_unmapped == blocks_per_page) {
      if (!bdev_write_page(bdev, blocks[0] << (blkbits - 9), page, wbc) ) {
        clean_buffers(page, first_unmapped);
        goto out;
      }
    }
#endif

    bdev = i->i_sb->s_bdev;
    if ( start_lbo >= sbi->dev_size ) {
      ufsd_printk( i->i_sb, "do_writepage: r=%lx, o=%llx, sz=%llx,%llx: start_lbo %llx >= dev_size %llx",
                   i->i_ino, page_off, u->valid, i->i_size, start_lbo, sbi->dev_size );
      BUG_ON( 1 );
    }

    mpage->bio = mpage_alloc( bdev, start_lbo >> 9, min_t( unsigned, bio_get_nr_vecs( bdev ), BIO_MAX_PAGES ) );
    if ( NULL == mpage->bio )
      goto out; // confused
  }

  assert( 0 != bh_off );
  if ( bio_add_page( mpage->bio, page, bh_off, 0 ) < bh_off ) {
    //
    // Looks like bio request is too big
    // Submit current bio and allocate new
    //
    ufsd_bio_write_submit( mpage->bio, sbi );
    goto alloc_new;
  }

  if ( unlikely( page_has_buffers( page ) ) ) {
    unsigned off = 0;
    bh = head  = page_buffers( page );
    do {
      if ( off == first_unmapped )
        break;
      off += blocksize;
      clear_buffer_dirty( bh );
    } while ( head != ( bh = bh->b_this_page ) );
  }

  BUG_ON( PageWriteback( page ) );
  set_page_writeback( page );
  unlock_page( page );

  if ( PAGE_SIZE == bh_off ) {
    mpage->next_lbo = start_lbo + PAGE_SIZE;
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> ok, next=%llx, sz=%llx,%llx", mpage->next_lbo, u->valid, i->i_size ));
  } else {
    ufsd_bio_write_submit( mpage->bio, sbi );
    mpage->bio = NULL;
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> ok (sumitted) sz=%llx,%llx", u->valid, i->i_size ));
  }

  ProfileLeave( sbi, do_writepage );
  return 0;

confused:
  if ( mpage->bio ) {
    ufsd_bio_write_submit( mpage->bio, sbi );
    mpage->bio = NULL;
  }

out:
  err = ufsd_buf_writepage( sbi, u, page, wbc, page_off, &map );
  if ( unlikely( err ) )
    mapping_set_error( mapping, err );

  ProfileLeave( sbi, do_writepage );

  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> %d (buf), sz=%llx,%llx", err, u->valid, i->i_size ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_writepage
//
// address_space_operations::writepage
///////////////////////////////////////////////////////////
static int
ufsd_writepage(
    IN struct page *page,
    IN struct writeback_control *wbc
    )
{
  struct address_space *mapping = page->mapping;
  struct inode  *i = mapping->host;
  usuper *sbi = UFSD_SB( i->i_sb );
  upage_data mpage;
  int err;

  DebugTrace( +1, Dbg, ("writepage: r=%lx, o=%llx", i->i_ino, (UINT64)page->index << PAGE_SHIFT) );

  ProfileEnter( sbi, writepage );

  // TODO: update 'arch' bit for exfat/ntfs
  mpage.bio  = NULL;

  err = ufsd_do_writepage( page, wbc, &mpage );
  if ( NULL != mpage.bio ) {
    ufsd_bio_write_submit( mpage.bio, sbi );
  }

  ProfileLeave( sbi, writepage );

  if ( likely( 0 == err ) ) {
    DebugTrace( -1, Dbg, ("writepage -> ok%s", mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( -1, Dbg, ("writepage -> err %d%s", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "failed to write page for inode 0x%lx, page index 0x%llx (error %d).", i->i_ino, (UINT64)page->index, err );
  }
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_write_begin
//
// fs/buffer.c block_write_begin + __block_write_begin
// address_space_operations::write_begin
///////////////////////////////////////////////////////////
static int
ufsd_write_begin(
    IN struct file    *file,
    IN struct address_space *mapping,
    IN loff_t         pos,
    IN unsigned       len,
    IN unsigned       flags,
    OUT struct page   **pagep,
    OUT void          **fsdata
    )
{
  int err = 0;
  struct buffer_head *bh, *head, *wait[2], **wait_bh=wait;
  struct inode *i = mapping->host;
  unode *u = UFSD_U( i );
  struct super_block *sb = i->i_sb;
  usuper *sbi   = UFSD_SB( sb );
  loff_t end    = pos + len;
  unsigned from = pos & (PAGE_SIZE - 1);
  unsigned to   = from + len;
  unsigned block_start;
  loff_t page_off = pos & ~(loff_t)( PAGE_SIZE - 1 );
  const unsigned blkbits    = i->i_blkbits;
  const unsigned blocksize  = 1 << blkbits;
  loff_t vbo, i_size, valid, i_size0 = i->i_size; // save original size
  struct page *page;
  mapinfo map;
  int dirty = 0;
  int PageUpt;

  DebugTrace( +1, UFSD_LEVEL_WBWE, ("write_begin: r=%lx, o=%llx,%x fl=%x sz=%llx,%llx%s",
                        i->i_ino, pos, len, flags, u->valid, i_size0, is_sparsed( u )?",sp":"" ));

  assert( Inode_is_locked( i ) );

  ProfileEnter( sbi, write_begin );

  if ( unlikely( end > i_size0 ) ) {
    err = ufsd_set_size_hlp( i, i_size0, end );
    if ( unlikely( 0 != err ) )
      goto restore;

    dirty   = 1;
    i_size  = end;
  } else {
    i_size  = i_size0;
  }

  {
    unsigned long lockf;
    write_lock_irqsave( &u->rwlock, lockf );
    if ( 0 != atomic_read( &u->write_begin_end_cnt ) ) {
      loff_t new_valid = page_off + PAGE_SIZE;
      if ( new_valid > u->valid )
        u->valid = new_valid;
    }
    valid = u->valid;
    write_unlock_irqrestore( &u->rwlock, lockf );
  }

  if ( !is_sparsed_or_compressed( u ) && unlikely( pos > valid ) ) {
    err = ufsd_extend_initialized_size( file, u, valid, pos );
    if ( unlikely( err < 0 ) )
      goto restore;
  }

  if ( dirty )
    mark_inode_dirty( i );

  // Do we really need to use 'AOP_FLAG_NOFS'?
  //page = grab_cache_page_write_begin( mapping, pos >> PAGE_CACHE_SHIFT, flags | AOP_FLAG_NOFS );
  page = grab_cache_page_write_begin( mapping, pos >> PAGE_SHIFT, flags );

  if ( unlikely( NULL == page ) ) {
    ufsd_printk( sb, "failed to allocate page cache page for inode 0x%lx at start 0x%llx.", i->i_ino, pos );
    err = -ENOMEM;
    goto restore;
  }

#ifdef UFSD_NTFS
  if ( is_compressed( u ) ) {
UseUfsd:
    *fsdata = (void*)(size_t)1;
    goto ok;
  }
  *fsdata = (void*)(size_t)0;
#endif

  PageUpt = PageUptodate( page );

#ifdef Try_to_writeback_inodes_sb
  if ( unlikely( sbi->options.wbMb_in_pages ) && !PageDirty( page ) ) {
    // Page is clear and will be used for write - increment counter
    atomic_inc( &sbi->dirty_pages_count );
  }
#endif

  if ( likely( !page_has_buffers( page ) ) ) {
#ifndef UFSD_USE_BH
    if ( !is_sparsed( u ) ) {
      if ( likely( 0 == from && PAGE_SIZE == to ) ) {
        DebugTrace( 0, UFSD_LEVEL_WBWE, ("full page" ));
        goto ok;
      }

      if ( unlikely( PageUpt ) ) {
        DebugTrace( 0, UFSD_LEVEL_WBWE, ("!full page + page_uptodate" ));
        goto ok;
      }
    }
#endif

    create_empty_buffers( page, blocksize, 0 );
    if ( !page_has_buffers( page ) ) {
      ufsd_printk( sb, "failed to allocate page buffers for inode 0x%lx.", i->i_ino );
      err = -ENOMEM;
      goto unlock_page;
    }
  }

  head    = page_buffers( page );
  vbo     = page_off;
  bh      = head;
  map.len = 0;
  block_start = 0;

  do {

    unsigned block_end = block_start + blocksize;

    if ( block_end <= from || block_start >= to ) {
      DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - out of request %llx + %x, bh: %lx%s", vbo, blocksize, bh->b_state, PageUpt? ",upt":"" ));
      if ( PageUpt && !buffer_uptodate( bh ) )
        set_buffer_uptodate( bh );
      goto next_block;
    }

    if ( vbo >= i_size ) {
      ufsd_printk( i->i_sb, "write_begin: r=%lx, pos=%llx, len=%x, o=%llx, sz=%llx,%llx,%llx",
                   i->i_ino, pos, len, vbo, u->valid, i_size0, i->i_size );
      BUG_ON( 1 );
    }

    if ( buffer_mapped( bh ) ) {
      DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - buffer mapped %llx, bh=%" PSCT "x", vbo, bh->b_blocknr ));
    } else {
      size_t to_map = (vbo + blocksize) > i_size? (i_size - vbo) : blocksize;
      //
      // Buffer is not mapped
      //
      bh->b_bdev = sb->s_bdev;

      if ( unlikely( map.len < to_map && ( vbo_to_lbo( sbi, u, vbo, to_map, &map ) || map.len < to_map ) ) ) {
        ufsd_printk( sb, "failed to map %llx of inode %lx, size=%llx,%llx", vbo, i->i_ino, u->valid, i_size );
        err = -EIO;
        break;
      }

#ifdef UFSD_NTFS
      if ( is_ntfs( &sbi->options ) ) {
        assert( UFSD_VBO_LBO_HOLE != map.lbo );
        assert( UFSD_VBO_LBO_COMPRESSED != map.lbo );

        if ( !is_lbo_ok( map.lbo ) ) {
          DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - use ufsd %llx", vbo ));
          goto UseUfsd;
        }

        if ( is_sparsed( u ) && FlagOn( map.flags, UFSD_MAP_LBO_NEW ) ) {
          bh->b_blocknr = map.lbo >> blkbits;
//          set_buffer_new( bh );
          clean_bdev_aliases( bh->b_bdev, bh->b_blocknr, 1 );
          set_buffer_mapped( bh );
          DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - set_mapped %llx => b=%" PSCT "x (new)", vbo, bh->b_blocknr ));
          if ( PageUpt ) {
  //          clear_buffer_new( bh );
            set_buffer_uptodate( bh );
            mark_buffer_dirty( bh );
            goto next_block;
          }

          // check if [pos,to) intersects with [vbo,block_end)
          if ( block_end > to || block_start < from ) {
            DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - zero out of new block [%x %x) [%x %x)", to, block_end, block_start, from ));
            zero_user_segments( page, to, block_end, block_start, from );
          }
          goto next_block;
        }
      }
#endif

      bh->b_blocknr = map.lbo >> blkbits;

      if ( map.lbo >= sbi->dev_size ) {
        ufsd_printk( i->i_sb, "write_begin: r=%lx, pos=%llx, len=%x, o=%llx, sz=%llx,%llx,%llx: bh=%" PSCT "x, lbo %llx,%llx >= dev_size %llx",
                     i->i_ino, pos, len, vbo, u->valid, i_size0, i->i_size, bh->b_blocknr, map.lbo, map.len, sbi->dev_size );
        BUG_ON( 1 );
      }

      DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - set_mapped %llx => b=%" PSCT "x", vbo, bh->b_blocknr ));
      set_buffer_mapped( bh );
    }

    if ( PageUpt ) {
      if ( !buffer_uptodate( bh ) )
        set_buffer_uptodate( bh );
      goto next_block;
    }

    if ( !buffer_uptodate( bh ) && (block_start < from || block_end > to) ) {
      valid = get_valid_size( u, NULL, NULL );
      if ( vbo < valid ) {
        DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - read %llx, b=%" PSCT "x", vbo, bh->b_blocknr ));
        Ll_rw_block( READ, 0, 1, &bh );
        *wait_bh++ = bh;
      } else {
        DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - zero_user %llx, b=%" PSCT "x + %lx", vbo, bh->b_blocknr, bh_offset( bh ) ));
        zero_user_segment( page, bh_offset( bh ), bh_offset( bh ) + blocksize );
        set_buffer_uptodate( bh );
      }
    }

next_block:
    if ( map.len < blocksize )
      map.len = 0;
    else {
      map.len -= blocksize;
      if ( is_lbo_ok( map.lbo ) )
        map.lbo += blocksize;
    }
    vbo += blocksize;
    block_start  = block_end;

  } while( head != (bh = bh->b_this_page) );

  //
  // If we issued read requests - let them complete.
  //
  while( unlikely( wait_bh > wait ) )  {
    bh = *--wait_bh;
    wait_on_buffer( bh );
    DebugTrace( 0, UFSD_LEVEL_WBWE, ("write_begin - wait %" PSCT "x, page_off=%llx, v=%llx", bh->b_blocknr, page_off, u->valid ));
    if ( !buffer_uptodate( bh ) ) {
      if ( !err )
        err = -EIO;
      ClearPageUptodate( page );
    } else {
      loff_t block_end = bh_offset( bh ) + page_off;

      if ( valid < block_end + blocksize ) {
        unsigned start = valid > block_end? (valid - block_end) : 0;
        unsigned start_page = start + bh_offset( bh );
        DebugTrace( 0, UFSD_LEVEL_WBWE, ( "write_begin - page_off=%llx, block_end=%llx, valid=%llx, zero_user_segment( %x, %lx )", page_off, block_end, valid, start_page, PAGE_SIZE ));
        zero_user_segment( page, start_page, PAGE_SIZE );
      }
    }
  }

  if ( likely( !err ) ) {
#if !defined UFSD_USE_BH || defined UFSD_NTFS
ok:
#endif
    *pagep = page;
  } else {
unlock_page:
    unlock_page( page );
    put_page( page );

restore:
    ufsd_printk( sb, "write_begin failed for inode 0x%lx, [%llx + %x), size=%llx,%llx, error %d).", i->i_ino, pos, len, u->valid, i->i_size, err );

    if ( i->i_size > i_size0 )
      ufsd_set_size( i, i->i_size, i_size0 );

    *pagep = NULL;
  }

  ProfileLeave( sbi, write_begin );

  DebugTrace( -1, UFSD_LEVEL_WBWE, ("write_begin -> %d, sz=%llx,%llx, pf=%lx", err, u->valid, i->i_size, NULL == *pagep? 0 : (*pagep)->flags ));

  if ( 0 == err )
    atomic_inc( &u->write_begin_end_cnt );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_write_end
//
// fs/buffer.c: generic_write_end
// address_space_operations::write_end
///////////////////////////////////////////////////////////
static int
ufsd_write_end(
    IN struct file  *file,
    IN struct address_space *mapping,
    IN loff_t       pos,
    IN unsigned     len,
    IN unsigned     copied,
    IN struct page  *page,
    IN void         *fsdata
    )
{
  struct inode *i = page->mapping->host;
  const unsigned blkbits = i->i_blkbits;
  unode *u            = UFSD_U( i );
  int i_size_changed;
  int PageUpt = PageUptodate( page );
#if defined UFSD_DEBUG || defined UFSD_NTFS
  usuper *sbi   = UFSD_SB( i->i_sb );
#endif
  unsigned long flags;
  int err         = copied >= len || PageUpt? copied : 0;
  loff_t end      = pos + err;
  loff_t page_off = (loff_t)page->index << PAGE_SHIFT;

  assert( copied == len ); // just to test
  assert( Inode_is_locked( i ) );

  assert( atomic_read( &u->write_begin_end_cnt ) >= 1 );
  atomic_dec( &u->write_begin_end_cnt );

  assert( copied <= len );
  assert( page->index == (pos >> PAGE_SHIFT) );

  DebugTrace( +1, UFSD_LEVEL_WBWE, ("write_end: r=%lx, pos=%llx,%x,%x s=%llx,%llx, pf=%lx",
              i->i_ino, pos, len, copied, u->valid, i->i_size, page->flags ));

  ProfileEnter( sbi, write_end );

  flush_dcache_page( page ); //??

#ifdef UFSD_NTFS
  assert( NULL == file || !is_stream( file ) );
  if ( NULL != fsdata ) {
    size_t ret;
    unsigned from   = pos & (PAGE_SIZE-1);
    unsigned to     = from + len;
    char* kaddr     = kmap( page );
    loff_t page_end = page_off + PAGE_SIZE;
    size_t towrite  = page_end > i->i_size? (i->i_size - page_off) : PAGE_SIZE;
    assert( NULL != sbi->rw_buffer );

    lock_ufsd( sbi );

    if ( PageUpt )
      err = 0;
    else if ( PAGE_SIZE == len ) {
      err = 0;
      SetPageUptodate( page );
    } else {

//      DebugTrace( 0, UFSD_LEVEL_WBWE, ("ufsdapi_file_read( o=%llx )", page_off ));
      err = ufsdapi_file_read( sbi->ufsd, u->ufile, NULL, 0, page_off, PAGE_SIZE, sbi->rw_buffer, &ret );

      if ( likely( 0 == err ) ) {
        if ( ret < PAGE_SIZE )
          memset( sbi->rw_buffer + ret, 0, PAGE_SIZE - ret );

        //
        // Update page
        //
        memcpy( kaddr, sbi->rw_buffer, from );
        memcpy( kaddr + to, sbi->rw_buffer + to, PAGE_SIZE - to );
        SetPageUptodate( page );

      } else {
        err = -EIO;
      }
    }

    if ( likely( 0 == err ) ) {
      err = ufsdapi_file_write( sbi->ufsd, u->ufile, NULL, 0, page_off, towrite, kaddr, &ret );
    }

    unlock_ufsd( sbi );

    kunmap( page );
    flush_dcache_page( page );

    if ( unlikely( 0 != err ) ) {
      SetPageError( page );
    } else {
      if ( PAGE_SIZE == towrite )
        ClearPageDirty( page ); //clear page dirty so that writepages wouldn't work for us

      if ( unlikely( page_has_buffers( page ) ) ) {
        struct buffer_head *bh, *head;

        head  = bh = page_buffers( page );
        do {
          set_buffer_uptodate( bh );
          clear_buffer_dirty( bh );
        } while ( head != (bh = bh->b_this_page) );
      }

      err = copied;
    }
  } else
#endif
  {
    // buffer.c: __block_commit_write
#ifndef UFSD_USE_BH
    if ( likely( !page_has_buffers( page ) ) ) {
      if ( likely( copied == len ) ) {
        if ( !PageUptodate( page ) )
          SetPageUptodate( page );
        set_page_dirty( page );
      }
    } else
#endif
    {
      struct buffer_head *bh, *head;
      loff_t block_start = page_off;
      const unsigned blocksize = 1 << blkbits;
      loff_t block_end = block_start + blocksize;
      int partial = 0;

      bh = head = page_buffers( page );

      do {
        if ( block_end <= pos || block_start >= end ) {
          if ( !buffer_uptodate( bh ) )
            partial = 1;
        } else {
          set_buffer_uptodate( bh );
          mark_buffer_dirty( bh );
        }
        block_start = block_end;
        block_end  += blocksize;
      } while ( head != ( bh = bh->b_this_page ) );

      if ( !partial && !PageUptodate( page ) )
        SetPageUptodate( page );
    }
  }

  write_lock_irqsave( &u->rwlock, flags );
  if ( end <= u->valid ) {
    i_size_changed = 0;
  } else {
    u->valid = end;
    if ( end > i->i_size )
      i_size_write( i, end );
    i_size_changed  = 1;
  }
  write_unlock_irqrestore( &u->rwlock, flags );

  unlock_page( page );
  put_page( page );

  if ( i_size_changed )
    mark_inode_dirty( i );

  if ( unlikely( copied != len ) ) {
    loff_t to = pos + len;
    BUG_ON( copied > len );
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_end - copied %x < len %x", copied, len ) );
    if ( err )
      ufsd_printk( i->i_sb, "partial write inode %lx: (orig copied %u, len %u, actual copied %u).", i->i_ino, copied, len, err );
    else
      ufsd_printk( i->i_sb, "write failed inode %lx: (orig copied %u, len %u, actual copied 0).", i->i_ino, copied, len );

    if ( to > i->i_size )
      ufsd_set_size( i, to, i->i_size );
  }

  ProfileLeave( sbi, write_end );

  DebugTrace( -1, UFSD_LEVEL_WBWE, (err > 0? "write_end -> %x s=%llx,%llx" : "write_end: -> %d s=%llx,%llx", err, u->valid, i->i_size) );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_readpages
//
// based on fs/mpage.c 'mpage_readpages'
//        mm/filemap.c 'add_to_page_cache_lru'
// address_space_operations::readpages
///////////////////////////////////////////////////////////
static int
ufsd_readpages(
    IN struct file          *file,
    IN struct address_space *mapping,
    IN struct list_head     *pages,
    IN unsigned             nr_pages
    )
{
  int err = 0;
  upage_data  mpage;
  struct inode *i = mapping->host;
  DEBUG_ONLY( usuper *sbi = UFSD_SB( i->i_sb ); )

  assert( 0 != nr_pages );
  if ( 0 == nr_pages )
    return 0;

  DebugTrace( +1, Dbg, ("readpages r=%lx, %llx + %x", i->i_ino, (UINT64)list_entry(pages->prev, struct page, lru)->index, nr_pages ));

  ProfileEnter( sbi, readpages );

  mpage.bio = NULL;

  for ( ; 0 != nr_pages; nr_pages-- ) {
    struct page *page = list_entry( pages->prev, struct page, lru );
    prefetchw( &page->flags );
    list_del( &page->lru );

//    if ( mapping_cap_swap_backed( mapping ) )
//      SetPageSwapBacked( page );

    if ( likely( 0 == add_to_page_cache( page, mapping, page->index, GFP_NOFS ) ) ) {
      int err2 = ufsd_do_readpage( page, nr_pages, &mpage );
      if ( unlikely( err2 ) ) {
        ufsd_printk( i->i_sb, "Failed to read page for inode 0x%lx, page index 0x%llx (error %d).", i->i_ino, (UINT64)page->index, err2 );
        if ( !err )
          err = err2;
      }

#if is_decl( LRU_CACHE_ADD )
      // 3.11+
//      lru_cache_add( page );
#else
      // lru_cache_add_anon may be declared but not exported (removed by linker?)
//      if ( PageSwapCache( page ) )
//        lru_cache_add_anon( page );
//      else
//        lru_cache_add_file( page );
#endif
      lru_cache_add_file( page );
    }

    put_page( page );
  }

  BUG_ON( !list_empty( pages ) );

  if ( mpage.bio )
    ufsd_bio_read_submit( mpage.bio );

  ProfileLeave( sbi, readpages );
  DebugTrace( -1, Dbg, ("readpages -> %d%s", err, mpage.bio? ", submitted":"" ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_writepages
//
// address_space_operations::writepages
// based on fs/mpage.c 'mpage_writepages'
///////////////////////////////////////////////////////////
static int
ufsd_writepages(
    IN struct address_space     *mapping,
    IN struct writeback_control *wbc
    )
{
  struct blk_plug plug;
  int err;
  struct inode* i = mapping->host;
  usuper *sbi = UFSD_SB( i->i_sb );
  CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
  upage_data mpage;

#ifdef UFSD_DEBUG
  // Save current 'nr_to_write' to show the number of written pages on exit
  long nr = wbc->nr_to_write;
  if ( ufsd_trace_level & Dbg ) {
    char tmp[32];
    const char* hint;
    if ( LONG_MAX == nr )
      hint = "all";
    else {
      sprintf( tmp, "%lx", nr );
      hint = tmp;
    }

    ufsd_trace( "%u: writepages r=%lx, %lx %s, \"%s\"", jiffies_to_msecs(jiffies-StartJiffies), i->i_ino, i->i_state, hint, current->comm );

    ufsd_trace_inc( +1 );
  }
#endif

  ProfileEnter( sbi, writepages );

  blk_start_plug( &plug );

  // TODO: update 'arch' bit for exfat/ntfs
  mpage.bio  = NULL;

  err = write_cache_pages( mapping, wbc, (writepage_t)ufsd_do_writepage, &mpage );

  if ( mpage.bio ) {
    ufsd_bio_write_submit( mpage.bio, sbi );
  }

  blk_finish_plug( &plug );

  ProfileLeave( sbi, writepages );

  if ( 0 == err ) {
    DebugTrace( -1, Dbg, ("%u: writepages -> ok, %lx%s", jiffies_to_msecs(jiffies-StartJiffies), nr - wbc->nr_to_write, mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( -1, Dbg, ("writepages -> fail %d%s", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "Failed due to error(s) for inode 0x%lx (error %d).", i->i_ino, err );
  }

  CheckTimeEx( 2, "%lx", nr - wbc->nr_to_write );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bmap
//
// address_space_operations::bmap
///////////////////////////////////////////////////////////
static sector_t
ufsd_bmap(
    IN struct address_space *mapping,
    IN sector_t block
    )
{
  sector_t ret = 0;
  struct inode *i = mapping->host;
  struct super_block *sb = i->i_sb;

  VfsTrace( +1, Dbg, ("bmap (%" PSCT "x)", block ));

  if ( S_ISDIR( i->i_mode ) ) {
    ufsd_printk( sb, "BMAP only makes sense for files, returning 0, inode 0x%lx", i->i_ino );
  } else {
    mapinfo map;
    unode *u  = UFSD_U( i );
    loff_t vbo = (loff_t)block << sb->s_blocksize_bits;

    //
    // map to read
    //
    if ( unlikely( vbo_to_lbo( UFSD_SB( i->i_sb ), u, vbo, 0, &map ) ) )
      ret = 0;
    else
      ret = map.lbo >> sb->s_blocksize_bits;
  }

  VfsTrace( -1, Dbg, ("bmap -> %" PSCT "x", ret ));
  return ret;
}


#if 0 // def UFSD_DEBUG
///////////////////////////////////////////////////////////
// ufsd_releasepage
//
// address_space_operations::releasepage
///////////////////////////////////////////////////////////
static int
ufsd_releasepage(
    IN struct page * page,
    IN gfp_t gfp_mask
    )
{
  struct address_space * const mapping = page->mapping;
  loff_t o = (loff_t)page->index << PAGE_CACHE_SHIFT;
  int ret = try_to_free_buffers( page );
  if ( NULL == mapping || NULL ==  mapping->host ) {
    DebugTrace( 0, Dbg, ("releasepage: o=%llx -> %d", o, ret ));
  } else {
    DebugTrace( 0, Dbg, ("releasepage: r=%lx, o=%llx -> %d", mapping->host->i_ino, o, ret ));
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_freepage
//
// address_space_operations::ufsd_aops
///////////////////////////////////////////////////////////
static void
ufsd_freepage(
    IN struct page * page
    )
{
  struct address_space * const mapping = page->mapping;
  loff_t o = (loff_t)page->index << PAGE_CACHE_SHIFT;
  if ( NULL == mapping || NULL == mapping->host ) {
    DebugTrace( 0, Dbg, ("freepage: o=%llx", o ));
  } else {
    DebugTrace( 0, Dbg, ("freepage: r=%lx, %llx", mapping->host->i_ino, o ));
  }
}
#define ufsd_releasepage  ufsd_releasepage
#define ufsd_freepage     ufsd_freepage
#endif


///////////////////////////////////////////////////////////
// ufsd_get_block_for_direct_IO
//
// call back function for blockdev_direct_IO
///////////////////////////////////////////////////////////
static int
ufsd_get_block_for_direct_IO(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  struct super_block  *sb = i->i_sb;
  usuper  *sbi            = UFSD_SB( sb );
  unode   *u              = UFSD_U( i );
  const unsigned blkbits  = i->i_blkbits;
  loff_t vbo = (loff_t)iblock << blkbits;
  mapinfo map;
  loff_t max_off = i->i_size, len = 0;
  int err = 0;
  DEBUG_ONLY( size_t bh_size = bh->b_size; )

  assert( 0 == bh->b_state );
  assert( 0 != bh->b_size );

  if ( !create && vbo >= get_valid_size( u, NULL, NULL ) ) {
    DebugTrace( 0, Dbg, ("get_block -> 0, r=%lx, %" PSCT "x -> out of valid", i->i_ino, iblock  ));
    return 0;
  }

  if ( vbo >= max_off )
    goto out;

  err = vbo_to_lbo( sbi, u, vbo, create? bh->b_size : 0, &map );
  if ( unlikely( err ) )
    goto out;

  assert( 0 != map.len );
  if ( unlikely( 0 == map.len ) )
    goto out;

  if ( vbo + map.len <= max_off )
    len = map.len;
  else
    len = (( max_off + sbi->cluster_mask ) & sbi->cluster_mask_inv) - vbo;

  if ( is_lbo_ok( map.lbo ) ) {
    bh->b_bdev    = sb->s_bdev;
    bh->b_blocknr = map.lbo >> blkbits;
    set_buffer_mapped( bh );
  }

  if ( create && is_sparsed( u ) && FlagOn( map.flags, UFSD_MAP_LBO_NEW ) ){
    set_buffer_new( bh );

    //
    // Do special action if new allocated cluster is bigger than block size
    //
    if ( sb->s_blocksize < sbi->cluster_mask ){
      //
      // Not effective but...
      //
      unsigned off = vbo & sbi->cluster_mask;
      loff_t off2;

      if ( 0 != off ) {
        //
        // Zero first part of cluster
        //
        ufsd_bd_zero( sb, map.lbo - off, off );
      }

      off2 = sbi->bytes_per_cluster - bh->b_size - off;
      if ( off2 > 0 ) {
        //
        // Zero second part of cluster
        //
        ufsd_bd_zero( sb, map.lbo + bh->b_size, off2 );
      }
    }
  }

out:

  //
  // get_block() is passed the number of i_blkbits-sized blocks which direct_io
  // has remaining to do.  The fs should not map more than this number of blocks.
  //
  // If the fs has mapped a lot of blocks, it should populate bh->b_size to
  // indicate how much contiguous disk space has been made available at
  // bh->b_blocknr.
  //
  // NOTE: 'bh->b_size' is size_t while 'len' if loff_t
  // 'bh->b_size' = len will fail in 32-bit
  //
  {
#ifdef __LP64__
    C_ASSERT( sizeof(size_t) == sizeof(loff_t) );
    bh->b_size = len;
#else
    C_ASSERT( sizeof(size_t) < sizeof(loff_t) );
    bh->b_size = len < 0x40000000u? len : 0x40000000u;
#endif
  }

  DebugTrace( 0, Dbg, ("get_block -> %d, r=%lx, %" PSCT "x -> [%" PSCT "x + %zx,%zx), %lx, %llx", err, i->i_ino, iblock, bh->b_blocknr, bh_size, bh->b_size, bh->b_state, max_off ));
  return err;
}


#if is_decl( BLOCKDEV_DIRECT_IO_V1 )
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      __blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block, NULL, NULL, DIO_LOCKING )
#elif is_decl( BLOCKDEV_DIRECT_IO_V2 )
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      __blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block, NULL, NULL, DIO_LOCKING )
#elif is_decl( BLOCKDEV_DIRECT_IO_V3 )
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      __blockdev_direct_IO( rw, iocb, i, bdev, iter, offset, get_block, NULL, NULL, DIO_LOCKING )
#elif is_decl( BLOCKDEV_DIRECT_IO_V4 )
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      __blockdev_direct_IO( iocb, i, bdev, iter, offset, get_block, NULL, NULL, DIO_LOCKING )
#elif is_decl( BLOCKDEV_DIRECT_IO_V5 )
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
     __blockdev_direct_IO( iocb, i, bdev, iter, get_block, NULL, NULL, DIO_LOCKING )
#endif


///////////////////////////////////////////////////////////
// ufsd_direct_IO
//
// address_space_operations::direct_IO
///////////////////////////////////////////////////////////
static ssize_t
ufsd_direct_IO(
#if !( is_decl( BLOCKDEV_DIRECT_IO_V4 ) ) \
 && !( is_decl( BLOCKDEV_DIRECT_IO_V5 ) )
    IN int                 rw,
#endif
    IN struct kiocb       *iocb
#if is_decl( BLOCKDEV_DIRECT_IO_V3 ) \
 || is_decl( BLOCKDEV_DIRECT_IO_V4 ) \
 || is_decl( BLOCKDEV_DIRECT_IO_V5 )
    // 3.16+
  , IN struct iov_iter    *iter
#if !( is_decl( BLOCKDEV_DIRECT_IO_V5 ) )
    // 4.6-
  , IN loff_t              offset
#endif
#else
  // 3.15-
  , IN const struct iovec *iov
  , IN loff_t              offset
  , IN unsigned long       nr_segs
#endif
    )
{
#ifndef UFSD_USE_AIO_READ_WRITE
  const struct iovec *iov     = iter->iov;
  unsigned long       nr_segs = iter->nr_segs;
#endif

#if is_decl( BLOCKDEV_DIRECT_IO_V4 ) || is_decl( BLOCKDEV_DIRECT_IO_V5 )
  int rw = iov_iter_rw(iter);
#endif

#if is_decl( BLOCKDEV_DIRECT_IO_V5 )
  loff_t offset = iocb->ki_pos;
#endif

  struct inode *i = iocb->ki_filp->f_mapping->host;
  struct block_device *bdev = i->i_sb->s_bdev;
  unode *u        = UFSD_U( i );
  loff_t valid, i_size, new_size;
  struct page **pages;
  ssize_t ret;

  VfsTrace( +1, Dbg, ("direct_IO: r=%lx, %s, %llx, %lu, s=%llx,%llx", i->i_ino,
              (rw&WRITE)? "w":"r", offset, nr_segs, u->valid, i->i_size ));

  if ( WRITE & rw ) {

    ret = Blockdev_direct_IO( WRITE, iocb, i, bdev, iov, offset, nr_segs, ufsd_get_block_for_direct_IO );
    if ( ret <= 0 )
      goto out;

    valid = get_valid_size( u, NULL, NULL );
    BUG_ON( !is_sparsed( u ) && offset > valid );

    new_size = offset + ret;
    if ( new_size > valid && !S_ISBLK( i->i_mode ) ) {
      set_valid_size( u, new_size );
      mark_inode_dirty( i );
    }

  } else {

    size_t uaddr, len, iov_off;
    unsigned long seg;
    loff_t tail_valid, done_read;
    const struct iovec *iov_last = iov + nr_segs;

    valid = get_valid_size( u, &i_size, NULL );

    if ( valid >= i_size || offset >= valid ) {
      ret = Blockdev_direct_IO( READ, iocb, i, bdev, iov, offset, nr_segs, ufsd_get_block_for_direct_IO );
      goto out;
    }

    tail_valid = valid - offset;

    //
    // How many segments until valid
    //
    for ( done_read = seg = 0; seg < nr_segs && done_read < tail_valid; seg += 1 ) {
      done_read += iov[seg].iov_len;
    }

    ret = Blockdev_direct_IO( READ, iocb, i, bdev, iov, offset, seg, ufsd_get_block_for_direct_IO );
    if ( ret < tail_valid )
      goto out;

    ret     = tail_valid;
    offset += ret;
    iov    += seg - 1;
    iov_off = iov->iov_len + tail_valid - done_read;
    uaddr   = iov_off + (size_t)iov->iov_base;
    len     = iov->iov_len - iov_off;

    //
    // Zero the rest of memory
    //
    pages = kmalloc( 64 * sizeof(struct page*), GFP_NOFS );
    if ( NULL == pages ) {
      ret = -ENOMEM;
      goto out;
    }

    for ( ;; ) {
      size_t nr_pages = ((uaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT) - (uaddr >> PAGE_SHIFT);

      while( nr_pages ) {
        long page_idx, mapped_pages;
        size_t to_zero;
        unsigned off_in_page;

        down_read( &current->mm->mmap_sem );
        mapped_pages = Get_user_pages( uaddr, min_t( unsigned long, nr_pages, 64 ), pages );
        up_read( &current->mm->mmap_sem );

        if ( unlikely( mapped_pages <= 0 ) ) {
          if ( !ret )
            ret = mapped_pages;
          goto end_zero;
        }

        nr_pages   -= mapped_pages;
        off_in_page = uaddr & ~PAGE_MASK;
        to_zero     = (mapped_pages << PAGE_SHIFT) - off_in_page;

        for ( page_idx = 0; page_idx < mapped_pages; page_idx++ ) {
          struct page *page = pages[page_idx];
          unsigned tail     = PAGE_SIZE - off_in_page;
          assert( 0 != len );
          if ( tail > len )
            tail = len;

          ret     += tail;
          offset  += tail;

          //
          // Zero full page after 'i_size'
          //
          zero_user_segment( page, off_in_page, offset >= i_size? PAGE_SIZE : off_in_page + tail );

          if ( offset >= i_size ) {
            ret -= offset - i_size;
            while( page_idx < mapped_pages )
              put_page( pages[page_idx++] );
            goto end_zero;
          }
          put_page( page );
          off_in_page  = 0;
          len -= tail;
        }

        assert( (0 == nr_pages) == ( 0 == len ) );
        uaddr += to_zero;
      }

      if ( ++iov >= iov_last )
        break;

      uaddr = (size_t)iov->iov_base;
      len   = iov->iov_len;
    }

end_zero:
    kfree( pages );
  }

out:
  VfsTrace( -1, Dbg, ("direct_IO -> %zx", ret ));

  return ret;
}


//
// Address space operations
//
static const struct address_space_operations ufsd_aops = {
  .writepage      = ufsd_writepage,
  .readpage       = ufsd_readpage,
#if is_struct( ADDRESS_SPACE_OPERATIONS_SYNC_PAGE )
  .sync_page      = block_sync_page,
#endif
  .writepages     = ufsd_writepages,
  .readpages      = ufsd_readpages,
  .write_begin    = ufsd_write_begin,
  .write_end      = ufsd_write_end,
  .bmap           = ufsd_bmap,
#ifdef ufsd_releasepage
  .releasepage    = ufsd_releasepage,
  .freepage       = ufsd_freepage,
#endif
  .direct_IO      = ufsd_direct_IO,
#if is_struct( ADDRESS_SPACE_OPERATIONS_MIGRATEPAGE )
  .migratepage    = buffer_migrate_page,
#endif
#if is_struct( ADDRESS_SPACE_OPERATIONS_INVALIDATEPAGE )
  .invalidatepage = block_invalidatepage,
#endif
#if is_struct( ADDRESS_SPACE_OPERATIONS_IS_PARTIALLY_UPTODATE )
  .is_partially_uptodate  = block_is_partially_uptodate,
#endif
#if is_struct( ADDRESS_SPACE_OPERATIONS_ERROR_REMOVE_PAGE )
  .error_remove_page  = generic_error_remove_page,
#endif
};


static struct kmem_cache *unode_cachep;

///////////////////////////////////////////////////////////
// ufsd_alloc_inode
//
// super_operations::alloc_inode
///////////////////////////////////////////////////////////
static struct inode*
ufsd_alloc_inode(
    IN struct super_block *sb
    )
{
  unode *u = kmem_cache_alloc( unode_cachep, GFP_NOFS );
  if ( NULL == u )
    return NULL;

  //
  // NOTE: explicitly zero all unode members from 'ufile' until 'i'
  //
  memset( &u->ufile, 0, offsetof(unode,i) - offsetof(unode,ufile) );

  return &u->i;
}


///////////////////////////////////////////////////////////
// ufsd_destroy_inode
//
// super_operations::destroy_inode
///////////////////////////////////////////////////////////
static void
ufsd_destroy_inode(
    IN struct inode *i
    )
{
  kmem_cache_free( unode_cachep, UFSD_U( i ) );
}


///////////////////////////////////////////////////////////
// init_once
//
// callback function for 'kmem_cache_create'
///////////////////////////////////////////////////////////
static void
init_once(
    IN void *foo
    )
{
  unode *u = (unode *)foo;

  //
  // NOTE: once init unode members from start to 'ufile'
  //
  rwlock_init( &u->rwlock );

  inode_init_once( &u->i );
}


///////////////////////////////////////////////////////////
// ufsd_symlink
//
// inode_operations::symlink
///////////////////////////////////////////////////////////
static int
ufsd_symlink(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN const char     *symname
    )
{
  struct inode *i;
  int err;
  ucreate  cr = { NULL, symname, strlen( symname ) + 1, 0, 0, S_IFLNK|S_IRWXUGO };

  VfsTrace( +1, Dbg, ("symlink: r=%lx, /\"%.*s\" => \"%s\"",
              dir->i_ino, (int)de->d_name.len, de->d_name.name, symname ));

  if ( unlikely( cr.len > PAGE_SIZE ) ) {
    DebugTrace( 0, Dbg, ("symlink name is too long" ));
    err = -ENAMETOOLONG;
    goto out;
  }

  if ( IS_ERR( i = ufsd_create_or_open( dir, de, &cr ) ) )
    err = PTR_ERR( i );
  else {
    err = 0;

    assert( NULL != UFSD_FH(i) );

#ifdef UFSD_HFS_ONLY
    // hfs+
    i->i_op = &ufsd_link_inode_operations_u8;
    Inode_lock( i );
    err = page_symlink( i, symname, cr.len );
    Inode_unlock( i );
#elif defined UFSD_HFS
    // hfs+/ntfs/exfat/refs
    if ( UFSD_SB( i->i_sb )->options.hfs ) {
      i->i_op = &ufsd_link_inode_operations_u8;
      Inode_lock( i );
      err = page_symlink( i, symname, cr.len );
      Inode_unlock( i );
    } else {
      i->i_op = &ufsd_link_inode_operations_ufsd;
    }
#else
    i->i_op = &ufsd_link_inode_operations_ufsd;
#endif

#if is_decl( INODE_NOHIGHMEM ) // 4.5+
    inode_nohighmem( i );
#endif

    if ( likely( 0 == err ) )
      d_instantiate( de, i );
    else
      drop_nlink( i );

    mark_inode_dirty( i );

    if ( unlikely( 0 != err ) )
      iput( i );
  }

out:
  VfsTrace( -1, Dbg, ("symlink -> %d", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_test_inode
//
// compare inodes for equality
// return 1 if match 'ino' and 'generation'
///////////////////////////////////////////////////////////
static int
ufsd_test_inode(
    IN struct inode     *i,
    IN ufsd_iget5_param *p
    )
{
  return i->i_ino == p->fi->Id && i->i_generation == p->fi->Gen;
}


///////////////////////////////////////////////////////////
// ufsd_set_inode
//
// initialize an inode
// return 0 if ok
///////////////////////////////////////////////////////////
static int
ufsd_set_inode(
    IN struct inode     *i,
    IN ufsd_iget5_param *p
    )
{
  unode *u                = UFSD_U( i );
  const ucreate *cr       = p->Create;
  const finfo *fi         = p->fi;
  struct super_block *sb  = i->i_sb;
  usuper *sbi             = UFSD_SB( sb );
  int check_special       = 0;
  mode_t mode;

  C_ASSERT( UFSD_UNODE_FLAG_SPARSE == UFSDAPI_SPARSE && UFSD_UNODE_FLAG_COMPRESS == UFSDAPI_COMPRESSED
         && UFSD_UNODE_FLAG_ENCRYPT == UFSDAPI_ENCRYPTED && UFSD_UNODE_FLAG_EA == UFSDAPI_EA );

  //
  // Next members are set at this point:
  //
  // i->i_sb    = sb;
  // i->i_dev   = sb->s_dev;
  // i->i_blkbits = sb->s_blocksize_bits;
  // i->i_flags = 0;
  //
//  assert( NULL == p->lnk );
  assert( 1 == atomic_read( &i->i_count ) );

  i->i_ino  = p->fi->Id;
  i->i_generation = p->fi->Gen;

  i->i_op = NULL;

  //
  // Setup 'uid' and 'gid'
  //
  i->i_uid = KUIDT_INIT( unlikely(sbi->options.uid)? sbi->options.fs_uid : cr? cr->uid : FlagOn( fi->Attrib, UFSDAPI_UGM )? fi->Uid : sbi->options.fs_uid );
  i->i_gid = KGIDT_INIT( unlikely(sbi->options.gid)? sbi->options.fs_gid : cr? cr->gid : FlagOn( fi->Attrib, UFSDAPI_UGM )? fi->Gid : sbi->options.fs_gid );

  //
  // Setup 'mode'
  //
  if ( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
    if ( sbi->options.dmask ) {
      // use mount options "dmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( FlagOn( fi->Attrib, UFSDAPI_UGM ) ) {
      // no mount options "dmask"/"umask" and fs supports "ugm"
      mode     = fi->Mode;
      check_special = 1;
    } else if ( NULL == sb->s_root ) {
      // Read root inode while mounting
      mode = S_IRWXUGO;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    }
  } else {
    if ( sbi->options.fmask ) {
      // use mount options "fmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( FlagOn( fi->Attrib, UFSDAPI_UGM ) ) {
      // no mount options "fmask"/"umask" and fs supports "ugm"
      mode     = fi->Mode;
      check_special = 1;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    }
  }

  if ( check_special && ( S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode) ) ) {
    init_special_inode( i, mode, new_decode_dev( fi->Dev ) );
    i->i_op = &ufsd_special_inode_operations;
  } else {
    assert( NULL == cr || !FlagOn( fi->Attrib, UFSDAPI_UGM ) || cr->mode == fi->Mode );
  }

  ufsd_times_to_inode( sbi, fi, i );
  i->i_size = fi->FileSize;

  //
  // Setup unode
  //
  u->flags = fi->Attrib & UFSD_UNODE_FLAG_API_FLAGS;

  u->valid = fi->ValidSize;
  assert( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) || !FlagOn( fi->Attrib, UFSDAPI_VSIZE ) || fi->ValidSize <= fi->FileSize );
  assert( NULL == p->fh || FlagOn( fi->Attrib, UFSDAPI_VSIZE ) );
//  BUG_ON( 0 != u->len );
//  assert( fi->ValidSize <= fi->FileSize );
//  assert( fi->FileSize <= fi->AllocSize );

#ifdef UFSD_NTFS
  if ( FlagOn( fi->Attrib, UFSDAPI_TASIZE ) ) {
    assert( is_sparsed_or_compressed( u ) );
    u->total_alloc = fi->TotalAllocSize;
  }
#endif

  inode_set_bytes( i, fi->AllocSize );

  if ( NULL != i->i_op ) {
    ;
  } else if ( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
    // dot and dot-dot should be included in count but was not included
    // in enumeration.
    assert( 1 == fi->HardLinks ); // Usually a hard link to directories are disabled
#ifdef UFSD_COUNT_CONTAINED
    set_nlink( i, fi->HardLinks + p->subdir_count + 1 );
#else
    set_nlink( i, 1 );
#endif
    i->i_op   = &ufsd_dir_inode_operations;
    i->i_fop  = &ufsd_dir_operations;
    mode      |= S_IFDIR;
    u->valid  = 0;
  } else {
    set_nlink( i, fi->HardLinks );

#if defined UFSD_USE_HFS_FORK && defined UFSD_HFS
#ifdef UFSD_HFS_ONLY
    // hfs+
    i->i_op = &ufsd_file_hfs_inode_ops;
#else
    // hfs+/ntfs/exfat
    i->i_op = sbi->options.hfs? &ufsd_file_hfs_inode_ops : &ufsd_file_inode_ops;
#endif
#else
    // ntfs/exfat or hfs on 3.13+
    i->i_op = &ufsd_file_inode_ops;
#endif

    i->i_fop    = &ufsd_file_ops;
    i->i_mapping->a_ops = &ufsd_aops;
    mode       |= S_IFREG;
  }

  if ( FlagOn( fi->Attrib, UFSDAPI_RDONLY ) )
    mode &= ~S_IWUGO;

  if ( FlagOn( fi->Attrib, UFSDAPI_LINK ) ) {
    // ntfs supports dir-symlinks but vfs preffers links to be files
    mode = ( mode & ~(S_IFDIR | S_IFREG) ) | S_IFLNK;

#ifdef UFSD_HFS_ONLY
    // hfs+
    i->i_op = &ufsd_link_inode_operations_u8;
#elif defined UFSD_HFS
    // hfs+/ntfs/exfat
    i->i_op = sbi->options.hfs? &ufsd_link_inode_operations_u8 : &ufsd_link_inode_operations_ufsd;
#else
    // ntfs/exfat
    i->i_op = &ufsd_link_inode_operations_ufsd;
#endif
    i->i_fop   = NULL;

#if is_decl( INODE_NOHIGHMEM ) // 4.5+
    inode_nohighmem( i );
#endif
  }

  if ( sbi->options.sys_immutable && FlagOn( fi->Attrib, UFSDAPI_SYSTEM )
    && !( S_ISFIFO(mode) || S_ISSOCK(mode) || S_ISLNK(mode) ) ) {
    DebugTrace( 0, 0, ("Set inode r=%lx, immutable", i->i_ino) );
    i->i_flags |= S_IMMUTABLE;
  } else {
    i->i_flags &= ~S_IMMUTABLE;
  }
#ifdef S_PRIVATE
  i->i_flags |= S_PRIVATE;  // ???
#endif

  i->i_mode = mode;

  assert( NULL == u->ufile );
  u->ufile  = p->fh;
//  *(struct inode**)Add2Ptr( u->ufile, usdapi_file_inode_offset() ) = i;

  if ( S_ISREG( i->i_mode ) )
    set_bit( UFSD_UNODE_FLAG_LAZY_INIT_BIT, &u->flags );

  p->fh     = NULL;

  return 0;
}


///////////////////////////////////////////////////////////
// iget5
//
// Helper function to get inode
///////////////////////////////////////////////////////////
static inline struct inode*
iget5(
    IN struct super_block *sb,
    IN OUT ufsd_iget5_param* p
    )
{
  struct inode *i =
    iget5_locked( sb, p->fi->Id,
                  (int (*)(struct inode *, void *))&ufsd_test_inode,
                  (int (*)(struct inode *, void *))&ufsd_set_inode,
                  p );

  if ( likely( NULL != i ) && FlagOn( i->i_state, I_NEW ) ) {
    unlock_new_inode( i );
  }

  return i;
}


///////////////////////////////////////////////////////////
// ufsd_create_or_open
//
//  This routine is a callback used to load or create inode for a
//  direntry when this direntry was not found in dcache or direct
//  request for create or mkdir is being served.
///////////////////////////////////////////////////////////
noinline static struct inode*
ufsd_create_or_open(
    IN struct inode       *dir,
    IN OUT struct dentry  *de,
    IN ucreate            *cr
    )
{
  ufsd_iget5_param param;
  unode *u        = NULL;
  struct inode *i = NULL;
  usuper *sbi     = UFSD_SB( dir->i_sb );
  int err = -ENOENT;
#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
  struct posix_acl *acl = NULL;
#endif
  TRACE_ONLY( const char *hint = NULL==cr?"lookup":S_ISDIR(cr->mode)?"mkdir":cr->lnk?"link":S_ISLNK(cr->mode)?"symlink":cr->data?"mknode":"create"; )
#ifdef UFSD_NTFS
  unsigned char *p = 0 == sbi->options.delim? NULL : strchr( de->d_name.name, sbi->options.delim );
  param.name_len      = NULL == p? de->d_name.len : p - de->d_name.name;
#else
  param.name_len      = de->d_name.len;
#endif

  if ( unlikely( !is_bdi_ok( dir->i_sb ) ) )
    return ERR_PTR( -ENODEV );

  param.Create        = cr;
  param.subdir_count  = 0;
  param.name          = de->d_name.name;

#if !is_struct( SUPER_BLOCK_S_D_OP )
  de->d_op = sbi->options.use_dop? &ufsd_dop : NULL;
#endif

  VfsTrace( +1, Dbg, ("%s: r=%lx, '%s' m=%o", hint, dir->i_ino, de->d_name.name, NULL == cr? 0u : (unsigned)cr->mode ));
//  DebugTrace( +1, Dbg, ("%s: %p '%.*s'", hint, dir, (int)param.name_len, param.name));

  //
  // The rest to be set in this routine
  // follows the attempt to open the file.
  //
  lock_ufsd( sbi );

  if ( unlikely( NULL != dir && 0 != ufsd_open_by_id( sbi, dir ) ) )
    goto Exit; // Failed to open parent directory

  if ( NULL != cr ) {
    struct inode *lnk = (struct inode*)cr->lnk;
    if ( NULL != lnk ) {
      if ( unlikely( 0 != ufsd_open_by_id( sbi, lnk ) ) )
        goto Exit; // Failed to open link node

      cr->lnk = UFSD_FH( lnk );
    }
    cr->uid = __kuid_val( current_fsuid() );
    if ( !(dir->i_mode & S_ISGID) )
      cr->gid = __kgid_val( current_fsgid() );
    else {
      cr->gid = __kgid_val( dir->i_gid );
      if ( S_ISDIR(cr->mode) )
        cr->mode |= S_ISGID;
    }

    if ( NULL == cr->lnk && !S_ISLNK(cr->mode) ) {
#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
      if ( sbi->options.acl ) {
        acl = ufsd_get_acl_ex( dir, ACL_TYPE_DEFAULT, 1 );
        if ( IS_ERR( acl ) ) {
          err = PTR_ERR( acl );
          acl = NULL;
          goto Exit;
        }
      }
      if ( NULL == acl )
#endif
        cr->mode &= ~current_umask();
    }
  }

  err = ufsdapi_file_open( sbi->ufsd, UFSD_FH( dir ), param.name, param.name_len,
                          cr,
#ifdef UFSD_COUNT_CONTAINED
                          &param.subdir_count,
#else
                          NULL,
#endif
                          &param.fh, &param.fi );
  if ( 0 != err )
    goto Exit;

  assert( NULL == cr || NULL != param.fh );
  assert( NULL != dir || FlagOn( param.fi->Attrib, UFSDAPI_SUBDIR ) ); // root must be directory

  //
  // Load and init inode
  // iget5 calls 'ufsd_set_inode' for new nodes
  // if node was not loaded then param.fh will be copied into UFSD_FH(inode)
  // and original param.fh will be zeroed
  // if node is already loaded then param.fh will not be changed
  // and we must to close it
  //
  i = iget5( dir->i_sb, &param );
  if ( unlikely( NULL == i ) ) {
    ufsdapi_file_close( sbi->ufsd, param.fh );
    err = -ENOMEM;
    goto Exit;
  }

  u = UFSD_U( i );

  if ( unlikely( NULL != param.fh ) ) {
    // inode was already opened
#ifdef UFSD_NTFS
    if ( NULL == u->ufile ) {
      DebugTrace( 0, Dbg, ("open closed r=%lx", i->i_ino ));
      u->ufile = param.fh;
    } else
#endif
    {
      DebugTrace( 0, Dbg, ("assert: i=%p, l=%x, old=%p, new=%p", i, i->i_nlink, u->ufile, param.fh ));
      // UFSD handle was not used. Close it
      ufsdapi_file_close( sbi->ufsd, param.fh );
    }
  }

  assert( NULL == cr || NULL != u->ufile );
  // OK
  err = 0;

  if ( NULL != cr ) {
    UINT64 dir_size = ufsdapi_get_dir_size( UFSD_FH( dir ) );

#ifdef UFSD_COUNT_CONTAINED
    if ( S_ISDIR ( i->i_mode ) )
      inc_nlink( dir );
#endif
    dir->i_mtime = dir->i_ctime = ufsd_inode_current_time( sbi );
    // Mark dir as requiring resync.
    i_size_write( dir, dir_size );
    inode_set_bytes( dir, dir_size );

    mark_inode_dirty( dir );

    if ( NULL != cr->lnk ) {
      i->i_ctime = dir->i_ctime;
    }
#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
    else if ( NULL != acl ) {
      posix_acl_mode mode = i->i_mode;

      if ( !S_ISDIR( mode ) || 0 == ( err = ufsd_set_acl_ex( i, acl, ACL_TYPE_DEFAULT, 1 ) ) ) {
#if is_decl( POSIX_ACL_CREATE_V1 ) || is_decl( POSIX_ACL_CREATE_V2 )
#if is_decl( POSIX_ACL_CREATE_V2 )
        err = __posix_acl_create( &acl, GFP_NOFS, &mode );
#else
        err = posix_acl_create( &acl, GFP_NOFS, &mode );
#endif
        if ( err >= 0 ) {
          if ( mode != i->i_mode ) {
            i->i_mode = mode;
            mark_inode_dirty( i );
          }
          if ( err > 0 )
            err = ufsd_set_acl_ex( i, acl, ACL_TYPE_ACCESS, 1 );
        }
#else
        struct posix_acl *clone = posix_acl_clone( acl, GFP_NOFS );
        if ( NULL == clone )
          err = -ENOMEM;
        else {
          err   = posix_acl_create_masq( clone, &mode );
          if ( err >= 0 ) {
            if ( mode != i->i_mode ) {
              i->i_mode = mode;
              mark_inode_dirty( i );
            }
            if ( err > 0 )
              err = ufsd_set_acl_ex( i, clone, ACL_TYPE_ACCESS, 1 );
          }
          ufsd_posix_acl_release( clone );
        }
#endif
      }
      if ( unlikely( err ) ) {
        iput( i );
        i = NULL;
      }
    }
#endif
  }

Exit:

  if ( 0 == err && sbi->options.no_acs_rules != u->stored_noacsr ) {
    if ( sbi->options.no_acs_rules ) {
      // "no access rules" mode and uid / gid / mode weren't saved
      u->i_mode = i->i_mode;
      u->i_uid  = i->i_uid;
      u->i_gid  = i->i_gid;
      i->i_mode |= UFSD_NOACSR_MODE;
      i->i_uid  = GLOBAL_ROOT_UID;
      i->i_gid  = GLOBAL_ROOT_GID;
      u->stored_noacsr = 1;
    } else {
      // normal mode and uid / gid / mode weren't restored
      i->i_mode = u->i_mode;
      i->i_uid  = u->i_uid;
      i->i_gid  = u->i_gid;
      u->stored_noacsr = 0;
    }
  }

  unlock_ufsd( sbi );
#if defined CONFIG_FS_POSIX_ACL && (defined UFSD_NTFS || defined UFSD_HFS)
  ufsd_posix_acl_release( acl );
#endif

  if ( 0 != err ) {
    assert( err < 0 );
    VfsTrace( -1, Dbg, ("%s failed %d", hint, err ));
    return ERR_PTR( err );
  }

  VfsTrace( -1, Dbg, ("%s -> i=%p de=%p h=%p r=%lx, l=%x m=%o%s",
                        hint, i, de, u->ufile,
                        i->i_ino, i->i_nlink, i->i_mode, FlagOn( param.fi->Attrib, UFSDAPI_SPARSE )?",sp" : FlagOn( param.fi->Attrib, UFSDAPI_COMPRESSED )?",c":""));
  return i;
}


#if defined CONFIG_PROC_FS

static struct proc_dir_entry *proc_info_root = NULL;
#define PROC_FS_UFSD_NAME "fs/"QUOTED_UFSD_DEVICE

#if !( is_decl( PDE_DATA ) )
  #define PDE_DATA(X) PDE(X)->data
#endif

///////////////////////////////////////////////////////////
// ufsd_proc_dev_version_show
//
// /proc/fs/ufsd/version
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_version_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  seq_printf( m, "%s%s\ndriver (%s) loaded at %p, sizeof(inode)=%u\n%s",
              ufsdapi_library_version( NULL ), s_FileVer, s_DriverVer, UFSD_MODULE_CORE(), (unsigned)sizeof(struct inode),
#ifdef DEFAULT_MOUNT_OPTIONS
              "Default options: " DEFAULT_MOUNT_OPTIONS "\n"
#endif
              "" );

#ifdef UFSD_HASH_VAL_H
  seq_printf( m, "Kernel .config hash: %s.\n", ufsd_hash_check_result );
#endif

#ifdef UFSD_DEBUG_ALLOC
  {
    size_t Mb = UsedMemMax/(1024*1024);
    size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
    if ( 0 != Mb ) {
      seq_printf( m, "Memory report: Peak usage %zu.%03Zu Mb (%zu bytes), kmalloc %zu, vmalloc %zu\n",
                  Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs );
    } else {
      seq_printf( m, "Memory report: Peak usage %zu.%03Zu Kb (%zu bytes), kmalloc %zu, vmalloc %zu\n",
                  Kb, UsedMemMax%1024, UsedMemMax, TotalKmallocs, TotalVmallocs );
    }
    seq_printf( m, "Total allocated:  %zu bytes in %zu blocks, Max request %zu bytes\n",
                  TotalAllocs, TotalAllocBlocks, MemMaxRequest );
  }
#endif

  return 0;
}

static int ufsd_proc_dev_version_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_version_show, NULL );
}

static const struct file_operations ufsd_proc_dev_version_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_version_open,
};


#define PROC_OPTIONS_MAX_LENGTH 128

static struct mutex   s_OptionsMutex;
static char ufsd_proc_mount_options[PROC_OPTIONS_MAX_LENGTH]
#ifdef DEFAULT_MOUNT_OPTIONS
     = DEFAULT_MOUNT_OPTIONS
#endif
    ;

///////////////////////////////////////////////////////////
// ufsd_proc_dev_options_show
//
// /proc/fs/ufsd/options
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_options_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  mutex_lock( &s_OptionsMutex );
  seq_printf( m, "%s\n", ufsd_proc_mount_options );
  mutex_unlock( &s_OptionsMutex );
  return 0;
}


static int ufsd_proc_dev_options_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_options_show, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_options_write
//
// /proc/fs/ufsd/options
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_options_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  size_t len = min_t( size_t, count, PROC_OPTIONS_MAX_LENGTH - 1 );
  ssize_t ret;

  mutex_lock( &s_OptionsMutex );
  if ( 0 != *ppos ) {
    ret = -EINVAL;
  } else if ( 0 != copy_from_user( ufsd_proc_mount_options, buffer, len ) ) {
    ret = -EINVAL;
//    ufsd_proc_mount_options[0] = 0; // ?
  } else {
    while( len > 0 && '\n' == ufsd_proc_mount_options[len-1] )
      len -= 1;
    ufsd_proc_mount_options[len] = 0; // always set last zero
    *ppos = count;
    ret   = count;// always return required
  }
  mutex_unlock( &s_OptionsMutex );

  return ret;
}


const struct file_operations ufsd_proc_dev_options_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_options_open,
  .write    = ufsd_proc_dev_options_write,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_dirty_show
//
// /proc/fs/ufsd/<dev>/dirty
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_dirty_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  struct super_block *sb = m->private;
  seq_printf( m, "%u\n", (unsigned)UFSD_SB( sb )->bdirty );
  return 0;
}

static int ufsd_proc_dev_dirty_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_dirty_show, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_dirty_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_dirty_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_volinfo
//
// /proc/fs/ufsd/<dev>/volinfo
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_volinfo(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  // int seq_printf() becomes void in 4.3+
  ufsdapi_trace_volume_info( sbi->ufsd, m, (SEQ_PRINTF)&seq_printf );

  unlock_ufsd( sbi );
  return 0;
}

static int ufsd_proc_dev_volinfo_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_volinfo, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_volinfo_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_volinfo_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_show
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_label_show(
    OUT struct seq_file *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );
  unsigned char *Label = kmalloc( PAGE_SIZE, GFP_NOFS );
  if ( NULL == Label )
    return -ENOMEM;

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  ufsdapi_query_volume_info( sbi->ufsd, NULL, Label, PAGE_SIZE, NULL );
  Label[PAGE_SIZE-1] = 0;

  unlock_ufsd( sbi );

  DebugTrace( 0, Dbg, ("read_label: %s", Label ) );

  seq_printf( m, "%s\n", Label );

  kfree( Label );
  return 0;
}

static int ufsd_proc_dev_label_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_label_show, PDE_DATA(inode) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_write
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_label_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  struct super_block *sb = PDE_DATA( file_inode( file ) );
  usuper *sbi = UFSD_SB( sb );
  // Always allocate additional byte for zero
  ssize_t allocate = (count + 1) < PAGE_SIZE ? (count + 1) : PAGE_SIZE;
  ssize_t ret = allocate - 1;
  char *Label = kmalloc( allocate, GFP_NOFS );
  if ( NULL == Label )
    return -ENOMEM;

  if ( copy_from_user( Label, buffer, ret ) ) {
    ret = -EFAULT;
  } else {
    // Remove last '\n'
    while( ret > 0 && '\n' == Label[ret-1] )
      ret -= 1;
    // Set last zero
    Label[ret] = 0;

    DebugTrace( 0, Dbg, ("write_label: %s", Label ) );

    //
    // Call UFSD library
    //
    lock_ufsd( sbi );

    ret = ufsdapi_set_volume_info( sbi->ufsd, Label, ret );

    unlock_ufsd( sbi );

    if ( 0 == ret ) {
      ret   = count; // Ok
      *ppos += count;
    } else {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_label failed: %x", (unsigned)ret ) );
      ret = -EINVAL;
    }
  }
  kfree( Label );
  return ret;
}

static const struct file_operations ufsd_proc_dev_label_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_label_open,
  .write    = ufsd_proc_dev_label_write,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_noacsr_show
//
// /proc/fs/ufsd/<dev>/noacsr
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_noacsr_show(
    OUT struct seq_file *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );
  char no_acs_rules = sbi->options.no_acs_rules;

  DebugTrace( 0, Dbg, ("read_noacsr: %d", no_acs_rules ) );

  seq_printf( m, "%d\n", no_acs_rules );

  return 0;
}


static int ufsd_proc_dev_noacsr_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_noacsr_show, PDE_DATA(inode) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_noacsr_write
//
// /proc/fs/ufsd/<dev>/noacsr
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_noacsr_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  char state_changed = 0;
  struct super_block *sb = PDE_DATA( file_inode( file ) );
  usuper *sbi = UFSD_SB( sb );
  ssize_t ret = count < PAGE_SIZE? count : PAGE_SIZE;
  char *no_acs_rules = kmalloc( ret, GFP_NOFS );
  if ( NULL == no_acs_rules )
    return -ENOMEM;

  if ( copy_from_user( no_acs_rules, buffer, ret ) ) {
    ret = -EFAULT;
  } else {
    if ( ( no_acs_rules[ 0 ] == '0' ) && ( sbi->options.no_acs_rules == 1 ) ) {
      // First symbol is '0' and option enabled - disable it
      sbi->options.no_acs_rules = 0;
      state_changed = 1;
    } else if ( ( no_acs_rules[ 0 ] != '0' ) && ( sbi->options.no_acs_rules == 0 ) ) {
      // First symbol isn't '0' and option disabled - enable it
      sbi->options.no_acs_rules = 1;
      state_changed = 1;
    }

    if ( state_changed ) {
      // Clear dentry cache for partition
      shrink_dcache_sb( sb );
      DebugTrace( 0, Dbg, ("write_noacsr: %d", sbi->options.no_acs_rules ) );
    }

    ret   = count; // Ok
    *ppos += count;
  }

  kfree( no_acs_rules );
  return ret;
}


static const struct file_operations ufsd_proc_dev_noacsr_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_noacsr_open,
  .write    = ufsd_proc_dev_noacsr_write,
};


#ifdef UFSD_DEBUG
static int ufsd_proc_dev_eject_open( struct inode *inode, struct file *file )
{
  return single_open( file, NULL, PDE_DATA(inode) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_eject_write
//
// /proc/fs/ufsd/<dev>/eject
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_eject_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  struct super_block *sb = PDE_DATA( file_inode( file ) );
  usuper *sbi = UFSD_SB( sb );
  sbi->eject = 1;
  ufsd_printk( sb, "ejected" );

  return 1;
}


static const struct file_operations ufsd_proc_dev_eject_fops = {
  .owner    = THIS_MODULE,
  .release  = single_release,
  .open     = ufsd_proc_dev_eject_open,
  .write    = ufsd_proc_dev_eject_write,
};
#endif // #ifdef UFSD_DEBUG


typedef struct {
  const char   name[8];
  const struct file_operations *fops;
  unsigned int mode;
} ufsd_proc_entries;

static const ufsd_proc_entries ProcInfoEntries[] = {
  { "dirty",    &ufsd_proc_dev_dirty_fops   , S_IFREG | S_IRUGO },
  { "label",    &ufsd_proc_dev_label_fops   , S_IFREG | S_IRUGO | S_IWUGO },
  { "volinfo",  &ufsd_proc_dev_volinfo_fops , S_IFREG | S_IRUGO },
  { "noacsr",   &ufsd_proc_dev_noacsr_fops  , S_IFREG | S_IRUGO | S_IWUGO },
#ifdef UFSD_DEBUG
  { "eject",    &ufsd_proc_dev_eject_fops   , S_IFREG | S_IWUGO },
#endif
};

static const ufsd_proc_entries ProcRootEntries[] = {
  { "version",  &ufsd_proc_dev_version_fops , S_IFREG | S_IRUGO },
  { "options",  &ufsd_proc_dev_options_fops , S_IFREG | S_IRUGO | S_IWUGO },
#ifdef UFSD_TRACE
  { "trace",    &ufsd_proc_dev_trace_fops   , S_IFREG | S_IRUGO | S_IWUGO },
  { "log",      &ufsd_proc_dev_log_fops     , S_IFREG | S_IRUGO | S_IWUGO },
  { "cycle",    &ufsd_proc_dev_cycle_fops   , S_IFREG | S_IRUGO | S_IWUGO },
#endif
};


///////////////////////////////////////////////////////////
// create_proc_entries
//
//
///////////////////////////////////////////////////////////
static const char*
create_proc_entries(
    IN const ufsd_proc_entries  *e,
    IN unsigned int             count,
    IN struct proc_dir_entry    *parent,
    IN void                     *data
    )
{
  for ( ; 0 != count--; e++ ) {
    if ( NULL == proc_create_data( e->name, e->mode, parent, e->fops, data ) )
      return e->name;
  }
  return NULL;
}


///////////////////////////////////////////////////////////
// remove_proc_entries
//
//
///////////////////////////////////////////////////////////
static void
remove_proc_entries(
    IN const ufsd_proc_entries  *e,
    IN unsigned int             count,
    IN struct proc_dir_entry    *parent
    )
{
  for ( ; 0 != count--; e++ )
    remove_proc_entry( e->name, parent );
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_create
//
// creates /proc/fs/ufsd/<dev>
// Called from 'ufsd_fill_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_create(
    IN struct super_block *sb
    )
{
  if ( NULL != proc_info_root ) {
    struct proc_dir_entry *e = proc_mkdir( sb->s_id, proc_info_root );
    const char *hint  = NULL == e? "" : create_proc_entries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), e, sb );
    if ( NULL != hint )
      printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s/%s\n", sb->s_id, hint );
    UFSD_SB( sb )->procdir = e;
  }
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_delete
//
// deletes /proc/fs/ufsd/<dev>
// Called from 'ufsd_put_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_delete(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );

  if ( NULL != sbi->procdir )
    remove_proc_entries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), sbi->procdir );

  if ( NULL != proc_info_root )
    remove_proc_entry( sb->s_id, proc_info_root );
  sbi->procdir = NULL;
}


///////////////////////////////////////////////////////////
// ufsd_proc_create
//
// creates "/proc/fs/ufsd"
// Called from 'ufsd_init'
///////////////////////////////////////////////////////////
static void
ufsd_proc_create( void )
{
  struct proc_dir_entry *e = proc_mkdir( PROC_FS_UFSD_NAME, NULL );
  const char *hint = NULL == e? "" : create_proc_entries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), e, NULL );
  if ( NULL != hint )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s\n", hint );
  proc_info_root = e;

  mutex_init( &s_OptionsMutex );
}


///////////////////////////////////////////////////////////
// ufsd_proc_delete
//
// deletes "/proc/fs/ufsd"
// Called from 'ufsd_exit'
///////////////////////////////////////////////////////////
static void
ufsd_proc_delete( void )
{
  if ( NULL != proc_info_root ) {
    remove_proc_entries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), proc_info_root );
    proc_info_root = NULL;
    remove_proc_entry( PROC_FS_UFSD_NAME, NULL );
  }
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  mutex_destroy( &s_OptionsMutex );
#endif
}

#else

  #define ufsd_proc_info_create( s )
  #define ufsd_proc_info_delete( s )
  #define ufsd_proc_create()
  #define ufsd_proc_delete()

#endif // #if defined CONFIG_PROC_FS


///////////////////////////////////////////////////////////
// ufsd_put_super
//
// super_operations::put_super
// Drop the volume handle.
///////////////////////////////////////////////////////////
static void
ufsd_put_super(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );
  VfsTrace( +1, Dbg, ("put_super: \"%s\"", sb->s_id));

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sbi );

  //
  // Stop flush thread
  //
#if UFSD_SMART_DIRTY_SEC
  write_lock( &sbi->state_lock );
  sbi->exit_flush_timer = 1;

  while ( NULL != sbi->flush_task ) {
    wake_up( &sbi->wait_exit_flush );
    write_unlock( &sbi->state_lock );
    wait_event( sbi->wait_done_flush, NULL == sbi->flush_task );
    write_lock( &sbi->state_lock );
  }
  write_unlock( &sbi->state_lock );
#endif

  // Remove /proc/fs/ufsd/..
  ufsd_proc_info_delete( sb );

  ufsdapi_volume_umount( sbi->ufsd );

  ufsd_uload_nls( &sbi->options );

#ifdef UFSD_NTFS
  if ( NULL != sbi->rw_buffer )
    vfree( sbi->rw_buffer );
#endif

#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  mutex_destroy( &sbi->api_mutex );
#endif

#if !defined UFSD_TRACE_SILENT && defined UFSD_DEBUG
  if ( ufsd_trace_level & UFSD_LEVEL_ERROR ) {
    ufsd_trace( "Delayed: %zu + %zu\n", sbi->nDelClear, sbi->nDelWrite );
    if ( sbi->nReadBlocks )
      ufsd_trace( "Read %zu, Written %zu\n", sbi->nReadBlocks, sbi->nWrittenBlocks );
    if ( sbi->nReadBlocksNa )
      ufsd_trace( "ReadNa %zu, WrittenNa %zu\n", sbi->nReadBlocksNa, sbi->nWrittenBlocksNa );
    if ( sbi->nMappedBh )
      ufsd_trace( "Mapped: %zu - %zu. Peek %zu\n", sbi->nMappedBh, sbi->nUnMappedBh, sbi->nPeakMappedBh );
    assert( sbi->nMappedBh == sbi->nUnMappedBh );
    if ( sbi->nCompareCalls )
      ufsd_trace( "ufsd_d_compare %zu/%zu\n", sbi->nCompareCalls, sbi->nCompareCallsUfsd );
    if ( sbi->nHashCalls )
      ufsd_trace("ufsd_d_hash %zu/%zu\n", sbi->nHashCalls, sbi->nHashCallsUfsd );
    if ( sbi->bdread_cnt )
      ufsd_trace("bdread       : %zu, %u msec\n", sbi->bdread_cnt, jiffies_to_msecs( sbi->bdread_ticks ) );
    if ( sbi->bdwrite_cnt )
      ufsd_trace("bdwrite      : %zu, %u msec\n", sbi->bdwrite_cnt, jiffies_to_msecs( sbi->bdwrite_ticks ) );
    if ( sbi->bdflush_cnt )
      ufsd_trace( "bdflush      : %zu, %u msec\n", sbi->bdflush_cnt, jiffies_to_msecs( sbi->bdflush_ticks ) );
    if ( sbi->bdmap_cnt )
      ufsd_trace("bdmap        : %zu, %u msec\n", sbi->bdmap_cnt, jiffies_to_msecs( sbi->bdmap_ticks ) );
    if ( sbi->bdsetdirty_cnt )
      ufsd_trace("bdsetdirty   : %zu, %u msec\n", sbi->bdsetdirty_cnt, jiffies_to_msecs( sbi->bdsetdirty_ticks ) );
    if ( sbi->bdunmap_meta_cnt )
      ufsd_trace("bdunmap_meta : %zu, %u msec, %zu\n", sbi->bdunmap_meta_cnt, jiffies_to_msecs( sbi->bdunmap_meta_ticks ), sbi->bdunmap_meta_sync );
    if ( sbi->bd_discard_cnt )
      ufsd_trace("bd_discard   : %zu, %u msec\n", sbi->bd_discard_cnt, jiffies_to_msecs( sbi->bd_discard_ticks ) );
    if ( sbi->bd_zero_cnt )
      ufsd_trace("bd_zero      : %zu, %u msec\n", sbi->bd_zero_cnt, jiffies_to_msecs( sbi->bd_zero_ticks ) );
    if ( sbi->readpage_cnt )
      ufsd_trace("readpage     : %zu, %u msec\n", sbi->readpage_cnt, jiffies_to_msecs( sbi->readpage_ticks ) );
    if ( sbi->readpages_cnt )
      ufsd_trace("readpages    : %zu, %u msec\n", sbi->readpages_cnt, jiffies_to_msecs( sbi->readpages_ticks ) );
    if ( sbi->do_readpage_cnt )
      ufsd_trace("do_readpage  : %zu, %u msec\n", sbi->do_readpage_cnt, jiffies_to_msecs( sbi->do_readpage_ticks ) );
    if ( sbi->buf_readpage_cnt )
      ufsd_trace("buf_readpage : %zu, %u msec\n", sbi->buf_readpage_cnt, jiffies_to_msecs( sbi->buf_readpage_ticks ) );
    if ( sbi->write_begin_cnt )
      ufsd_trace("write_begin  : %zu, %u msec\n", sbi->write_begin_cnt, jiffies_to_msecs( sbi->write_begin_ticks ) );
    if ( sbi->write_end_cnt )
      ufsd_trace("write_end    : %zu, %u msec\n", sbi->write_end_cnt, jiffies_to_msecs( sbi->write_end_ticks ) );
    if ( sbi->writepage_cnt )
      ufsd_trace("writepage    : %zu, %u msec\n", sbi->writepage_cnt, jiffies_to_msecs( sbi->writepage_ticks ) );
    if ( sbi->writepages_cnt )
      ufsd_trace("writepages   : %zu, %u msec\n", sbi->writepages_cnt, jiffies_to_msecs( sbi->writepages_ticks ) );
    if ( sbi->do_writepage_cnt )
      ufsd_trace("do_writepage : %zu, %u msec\n", sbi->do_writepage_cnt, jiffies_to_msecs( sbi->do_writepage_ticks ) );
    if ( sbi->buf_writepage_cnt )
      ufsd_trace("buf_writepage: %zu, %u msec\n", sbi->buf_writepage_cnt, jiffies_to_msecs( sbi->buf_writepage_ticks ) );
    if ( sbi->write_inode_cnt )
      ufsd_trace("write_inode  : %zu, %u msec\n", sbi->write_inode_cnt, jiffies_to_msecs( sbi->write_inode_ticks ) );
#if defined UFSD_REFS || defined UFSD_REFS3
    if ( sbi->buf_get_cnt )
      ufsd_trace("buf_get      : %zu, %u msec\n", sbi->buf_get_cnt, jiffies_to_msecs( sbi->buf_get_ticks ) );
    if ( sbi->buf_put_cnt )
      ufsd_trace("buf_put      : %zu, %u msec\n", sbi->buf_put_cnt, jiffies_to_msecs( sbi->buf_put_ticks ) );
    if ( sbi->buf_write_cnt )
      ufsd_trace("buf_write    : %zu, %u msec\n", sbi->buf_write_cnt, jiffies_to_msecs( sbi->buf_write_ticks ) );
#endif
  }
#endif

#ifdef CONFIG_FS_POSIX_ACL
  if ( NULL != sbi->x_buffer )
    kfree( sbi->x_buffer );
#endif

  ufsd_heap_free( sbi );
  sb->s_fs_info = NULL;
  assert( NULL == UFSD_SB( sb ) );

  sync_blockdev( sb->s_bdev );
  invalidate_bdev( sb->s_bdev );

#if defined UFSD_DEBUG_ALLOC & !defined UFSD_TRACE_SILENT
  trace_mem_report( 0 );
#endif

  VfsTrace( -1, Dbg, ("put_super ->\n"));
}


///////////////////////////////////////////////////////////
// ufsd_write_inode
//
// super_operations::write_inode
///////////////////////////////////////////////////////////
static int
ufsd_write_inode(
    IN struct inode *i,
    IN struct writeback_control *wbc
    )
{
  int err     = 0;
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  DEBUG_ONLY( const char* hint; )

  DebugTrace( +1, Dbg, ("write_inode: r=%lx, %s, s=%d, \"%s\"", i->i_ino, S_ISDIR( i->i_mode )? "dir" : "file", (int)wbc->sync_mode, current->comm));

  ProfileEnter( sbi, write_inode );

  if ( unlikely( NULL == u->ufile ) ) {
    DebugTrace( 0, Dbg, ("write_inode: no ufsd handle for this inode"));
    DEBUG_ONLY( hint = "no file"; )
//    err = -EBADF;
  } else if ( !Inode_trylock( i ) ) {
//    mark_inode_dirty_sync( i );
    DEBUG_ONLY( hint = "file locked"; )
  } else {
    UINT64 allocated;
    const UINT64 *asize = NULL;
    if ( try_lock_ufsd( sbi ) ) {
      if ( likely( NULL != u->ufile )
        && 0 == ufsdapi_file_flush( sbi->ufsd, u->ufile, sbi->fi, ufsd_update_ondisk( sbi, i, sbi->fi ),
                                    i, atomic_read( &i->i_writecount ) > 0, &allocated ) ) {
        asize = &allocated;
      }

      DEBUG_ONLY( hint = "ok"; )
      unlock_ufsd( sbi );
    } else {
      //
      // Add this inode to internal list to write later
      //
      delay_write_inode *dw = kmalloc( sizeof(delay_write_inode), GFP_NOFS | __GFP_ZERO );
      if ( NULL == dw )
        err = -ENOMEM;
      else {
        dw->ia_valid  = ufsd_update_ondisk( sbi, i, &dw->fi );
        dw->ufile     = u->ufile;
        spin_lock( &sbi->ddt_lock );
        list_add( &dw->wlist, &sbi->write_list );
        spin_unlock( &sbi->ddt_lock );
        DEBUG_ONLY( sbi->nDelWrite += 1; )
      }
      DEBUG_ONLY( hint = "ufsd locked"; )
    }

    if ( !is_compressed( u ) )
      update_cached_size( sbi, u, i->i_size, asize );

    Inode_unlock( i );
  }

  ProfileLeave( sbi, write_inode );

  DebugTrace( -1, Dbg, ("write_inode -> %s", hint));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_sync_volume
//
// super_operations::sync_fs
///////////////////////////////////////////////////////////
static int
ufsd_sync_volume(
    IN struct super_block *sb,
    IN int wait
    )
{
  usuper *sbi = UFSD_SB( sb );
  VfsTrace( +1, Dbg, ("sync_volume: \"%s\"%s", sb->s_id, wait? ",w":""));

  sbi->bdirty = 0;

  SMART_TRACE_ONLY( printk( "<4>ufsd: sync_volume:+\n" ); )

  if ( try_lock_ufsd( sbi ) ) {
    CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
    ufsdapi_volume_flush( sbi->ufsd, wait );
    unlock_ufsd( sbi );
    CheckTime( 2 );
    ufsd_check_sp();

  } else {

    //
    // Do volume flush later
    //
    atomic_set( &sbi->VFlush, wait? 2 : 1 );
  }

  SMART_TRACE_ONLY( printk( "<4>ufsd: sync_volume:-\n" ); )

  VfsTrace( -1, Dbg, ("sync_volume ->"));
  return 0;
}


#if UFSD_SMART_DIRTY_SEC
///////////////////////////////////////////////////////////
// ufsd_add_timer
//
// Helper function to add timer UFSD_SMART_DIRTY_SEC after last dirty
///////////////////////////////////////////////////////////
static inline void
ufsd_add_timer(
    IN usuper       *sbi,
    IN unsigned int sec
    )
{
  mod_timer( &sbi->flush_timer, HZ + sbi->last_dirty + msecs_to_jiffies( sec * 1000 ) );
}


///////////////////////////////////////////////////////////
// flush_timer_fn
//
// Timer function
///////////////////////////////////////////////////////////
static void
flush_timer_fn(
    IN unsigned long data
    )
{
  usuper *sbi = (usuper*)data;

  if ( !sbi->bdirty ) {
    // Do not wake up flush thread
  } else {
#if UFSD_SMART_DIRTY_SEC
    long dj = jiffies - sbi->last_dirty;
    if ( dj <= 0 || jiffies_to_msecs( dj ) < UFSD_SMART_DIRTY_SEC * 1000 ) {
      // Do not wake up flush thread
      // Sleep for another period
      ufsd_add_timer( sbi, UFSD_SMART_DIRTY_SEC );
    } else
#endif
    if ( NULL != sbi->flush_task ) {
      //
      // Volume is dirty and there are no writes last UFSD_SMART_DIRTY_SEC
      // Wake up flush thread
      //
      wake_up_process( sbi->flush_task );
    }
  }
}


///////////////////////////////////////////////////////////
// ufsd_flush_thread
//
// 'dirty_writeback_interval'
///////////////////////////////////////////////////////////
static int
ufsd_flush_thread(
    IN void *arg
    )
{
  struct super_block *sb = arg;
  usuper *sbi = UFSD_SB( sb );
#ifdef UFSD_DEBUG
  unsigned long j0, j1, j_a = 0, j_s = 0, cnt = 0;
#endif

  // Record that the flush thread is running
  sbi->flush_task = current;

  //
  // Set up an interval timer which can be used to trigger a flush wakeup after the flush interval expires
  //
  setup_timer( &sbi->flush_timer, flush_timer_fn, (unsigned long)sbi );

  wake_up( &sbi->wait_done_flush );

  set_freezable();

  //
  // And now, wait forever for flush wakeup events
  //
  write_lock( &sbi->state_lock );

  DEBUG_ONLY( j0 = jiffies; )

  for ( ;; ) {
    if ( sbi->exit_flush_timer ) {
      write_unlock( &sbi->state_lock );
      del_timer_sync( &sbi->flush_timer );
      sbi->flush_task = NULL;
      wake_up( &sbi->wait_done_flush );
      DebugTrace( 0, Dbg, ("flush_thread exiting: active %u, sleep %u, cycles %lu", jiffies_to_msecs( j_a ), jiffies_to_msecs( j_s ), cnt ));
      return 0;
    }

    if ( sbi->bdirty ) {
      long dj = jiffies - sbi->last_dirty;
      if ( dj <= 0 || jiffies_to_msecs( dj ) < UFSD_SMART_DIRTY_SEC * 1000 ) {
        ufsd_add_timer( sbi, UFSD_SMART_DIRTY_SEC );
        SMART_TRACE_ONLY( printk( KERN_WARNING QUOTED_UFSD_DEVICE": flush_thread: skip\n" ); )
      } else {
        DEBUG_ONLY( const char *hint;  )
        sbi->bdirty = 0;
        write_unlock( &sbi->state_lock );

        SMART_TRACE_ONLY( printk( KERN_WARNING QUOTED_UFSD_DEVICE": flush_thread:+\n" ); )
        DebugTrace( +1, Dbg, ("flush_thread: \"%s\"", sb->s_id));

        //
        // Sync user's data
        //
        if ( likely( down_read_trylock( &sb->s_umount ) ) ) {
          if ( likely( is_bdi_ok( sb ) ) ) {
//            DEBUG_ONLY( struct timeval tv; )
//            DEBUG_ONLY( do_gettimeofday( &tv ); )
//            DebugTrace( 0, UFSD_LEVEL_WBWE, ( "%ld.%ld ufsd_flush_thread begin inode flush", tv.tv_sec, tv.tv_usec ));
            sync_inodes_sb( sb );
//            DEBUG_ONLY( do_gettimeofday( &tv ); )
//            DebugTrace( 0, UFSD_LEVEL_WBWE, ( "%ld.%ld ufsd_flush_thread end inode flush", tv.tv_sec, tv.tv_usec ));
          }
          up_read( &sb->s_umount );
        }

        if ( down_write_trylock( &sb->s_umount ) ) {
          if ( try_lock_ufsd( sbi ) ) {
            CHECK_TIME_ONLY( unsigned long j0 = jiffies; )
            ufsdapi_volume_flush( sbi->ufsd, 1 );
            CheckTime( 2 );
            ufsd_check_sp();
            unlock_ufsd( sbi );
            DEBUG_ONLY( hint = "flushed"; )
          } else {
            //
            // Do volume flush later
            //
            atomic_set( &sbi->VFlush, 1 );
            DEBUG_ONLY( hint = "delay"; )
          }
          up_write( &sb->s_umount );
        } else {
          atomic_set( &sbi->VFlush, 1 );
          DEBUG_ONLY( hint = "delay"; )
        }
        SMART_TRACE_ONLY( printk( KERN_WARNING QUOTED_UFSD_DEVICE": flush_thread:-\n" ); )
        DebugTrace( -1, Dbg, ("flush_thread -> %s", hint));

        write_lock( &sbi->state_lock );
      }
    }

    wake_up( &sbi->wait_done_flush );

#ifdef UFSD_DEBUG
    cnt += 1;
    j1 = jiffies;
    j_a += j1 - j0;
    j0 = j1;
#endif

    if ( freezing( current ) ) {
      DebugTrace( 0, Dbg, ("now suspending flush_thread" ));
      write_unlock( &sbi->state_lock );
#if is_decl( REFRIGERATOR )
      refrigerator();
#else
      try_to_freeze();
#endif
      write_lock( &sbi->state_lock );

    } else if ( !sbi->exit_flush_timer ) {

      DEFINE_WAIT( wait );
      prepare_to_wait( &sbi->wait_exit_flush, &wait, TASK_INTERRUPTIBLE );
      write_unlock( &sbi->state_lock );

      schedule();

#ifdef UFSD_DEBUG
      j1 = jiffies;
      j_s += j1 - j0;
      j0 = j1;
#endif

      write_lock( &sbi->state_lock );
      finish_wait( &sbi->wait_exit_flush, &wait );
    }
  }
}
#endif


///////////////////////////////////////////////////////////
// ufsd_on_set_dirty
//
// Callback function. Called when volume becomes dirty
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_on_set_dirty(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );

#if UFSD_SMART_DIRTY_SEC
  write_lock( &sbi->state_lock );
  sbi->last_dirty = jiffies;
  sbi->bdirty = 1;
  if ( NULL != sbi->flush_timer.function ) // check case when this function is called while mounting
    ufsd_add_timer( sbi, UFSD_SMART_DIRTY_SEC );
  write_unlock( &sbi->state_lock );

  DebugTrace( 0, Dbg, ("ufsd_on_set_dirty( %u )", jiffies_to_msecs(jiffies-StartJiffies) + UFSD_SMART_DIRTY_SEC * 1000 ));

#else

  sbi->bdirty = 1;

#endif
  assert( !(sb->s_flags & MS_RDONLY) );
}


///////////////////////////////////////////////////////////
// ufsd_statfs
//
// super_operations::statfs
///////////////////////////////////////////////////////////
static int
ufsd_statfs(
    IN struct dentry    *de,
    OUT struct kstatfs  *buf
    )
{
  struct super_block *sb = de->d_sb;
  usuper *sbi = UFSD_SB( sb );
  ufsd_volume_info info;
  UINT64 free_clusters;
  DebugTrace( +1, Dbg, ("statfs: \"%s\"", sb->s_id));
  lock_ufsd( sbi );

  ufsdapi_query_volume_info( sbi->ufsd, &info, NULL, 0, &free_clusters );

  unlock_ufsd( sbi );

  buf->f_type   = info.fs_signature;
  buf->f_bsize  = info.bytes_per_cluster;
  buf->f_blocks = info.total_clusters;
  buf->f_bfree  = free_clusters;
  buf->f_bavail = buf->f_bfree;
  buf->f_files  = 0;
  buf->f_ffree  = 0;
  buf->f_namelen= info.namelen;

  DebugTrace( -1, Dbg, ("statfs -> free=%llx", free_clusters));
  //TRACE_ONLY(show_buffers();)
#if defined UFSD_DEBUG_ALLOC & !defined UFSD_TRACE_SILENT
  trace_mem_report( 0 );
#endif
  return 0;
}

// Forward declaration
static const char*
ufsd_parse_options(
    IN usuper *sbi,
    IN char   *options,
    IN int    first_mount
    );


///////////////////////////////////////////////////////////
// ufsd_remount
//
// super_operations::remount_fs
///////////////////////////////////////////////////////////
static int
ufsd_remount(
    IN struct super_block *sb,
    IN int                *flags,
    IN char               *data
    )
{
  mount_options opts_saved;
  int err = -EINVAL;
  int NeedParse = NULL != data && 0 != data[0];
  int Ro = *flags & MS_RDONLY;
  ufsd_volume_info info;
  usuper *sbi = UFSD_SB( sb );
  C_ASSERT( sizeof(sbi->options) == sizeof(opts_saved) );

#ifdef UFSD_TRACE
  unsigned long new_ufsd_trace_level, prev_ufsd_trace_level;

  mutex_lock( &s_MountMutex );

  // Save current trace level
  new_ufsd_trace_level  = prev_ufsd_trace_level = ufsd_trace_level;
#endif

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  VfsTrace( +1, Dbg, ("remount \"%s\", %lx, options \"%s\"", sb->s_id, sb->s_flags, data ));

  if ( (sb->s_flags & MS_RDONLY) && !Ro && sbi->options.journal >= JOURNAL_STATUS_NEED_REPLAY ) {
    DebugTrace( 0, Dbg, ("remount \"%s\": ro -> rw + jnl", sb->s_id ));
    printk( KERN_WARNING QUOTED_UFSD_DEVICE ": Couldn't remount \"%s\" rw because journal is not replayed."
            " Please umount/remount instead\n", sb->s_id );
    NeedParse = 0;
    goto Exit;
  }

  if ( NeedParse ) {
    const char* parse_err;

    // Save current options
    memcpy( &opts_saved, &sbi->options, sizeof(opts_saved) );

    // Parse options passed in command 'mount'
    memset( &sbi->options, 0, sizeof(opts_saved) );

    parse_err = ufsd_parse_options( sbi, data, 1 );
    if ( NULL != parse_err ) {
      VfsTrace( 0, Dbg, ("remount: failed to remount \"%s\", bad options \"%s\"", sb->s_id, parse_err ));
      goto Exit;
    }

#ifdef UFSD_TRACE
    // Save new value of trace and restore previous until the end of this function
    new_ufsd_trace_level  = ufsd_trace_level;
    ufsd_trace_level      = prev_ufsd_trace_level;
#endif
  }

  *flags |= MS_NODIRATIME | (sbi->options.noatime? MS_NOATIME : 0);

  if ( !Ro
    && ( 0 != ufsdapi_query_volume_info( sbi->ufsd, &info, NULL, 0, NULL )
      || 0 != info.dirty )
    && !sbi->options.force ) {
    //
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
    goto Exit;
  }

  err = ufsdapi_volume_remount( sbi->ufsd, &Ro, &sbi->options );
  if ( 0 != err ) {
    DebugTrace( 0, Dbg, ("remount: failed to remount \"%s\", ufsdapi_volume_remount failed %x", sb->s_id, (unsigned)err ));
    err = -EINVAL;
    goto Exit;
  }

  if ( NeedParse ) {
    // unload original nls
    ufsd_uload_nls( &opts_saved );
  }

#if is_struct( SUPER_BLOCK_S_D_OP )
  sb->s_d_op = sbi->options.use_dop? &ufsd_dop : NULL;
#endif

  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_SHIFT-10 );

  if ( Ro )
    sb->s_flags |= MS_RDONLY;
  else
    sb->s_flags &= ~MS_RDONLY;

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

Exit:

  if ( 0 != err && NeedParse ) {
    // unload new nls
    ufsd_uload_nls( &sbi->options );
    // Restore original options
    memcpy( &sbi->options, &opts_saved, sizeof(opts_saved) );
  }

  unlock_ufsd( sbi );

  VfsTrace( -1, Dbg, ("remount -> %d", err ));

#ifdef UFSD_TRACE
  // Setup new trace level
  ufsd_trace_level = new_ufsd_trace_level;

  mutex_unlock( &s_MountMutex );
#endif

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_evict_inode
//
// super_operations::evict_inode
///////////////////////////////////////////////////////////
static void
ufsd_evict_inode(
    IN struct inode *i
    )
{
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  TRACE_ONLY( const char *hint=""; )

  VfsTrace( +1, Dbg, ("evict_inode: r=%lx, h=%p", i->i_ino, u->ufile ));

  //
  // wait pending io operations to be finished ( 0 == u->ioend_count )
  //
#if is_decl( TRUNCATE_INODE_PAGES_FINAL )
  truncate_inode_pages_final( &i->i_data );
#elif is_decl( TRUNCATE_INODE_PAGES )
  if ( i->i_data.nrpages )
    truncate_inode_pages( &i->i_data, 0 );
#else
  #error "truncate_inode_pages or truncate_inode_pages_final not defined"
#endif

#if is_decl( END_WRITEBACK )
  end_writeback( i );
#elif is_decl( CLEAR_INODE )
  //In kernel 3.5 end_writeback renamed to clear_inode
  clear_inode( i );
#else
#error "end_writeback or clear_inode not defined"
#endif

  if ( NULL == sbi ) {
    TRACE_ONLY( hint="forgotten"; )
  } else {
    ufsd_file *file;
    spin_lock( &i->i_lock );
    file      = u->ufile;
    u->ufile  = NULL;
    spin_unlock( &i->i_lock );

    if ( NULL != file ) {
      if ( try_lock_ufsd( sbi ) ) {
        ufsdapi_file_close( sbi->ufsd, file );
        unlock_ufsd( sbi );
      } else {
        struct list_head* lh = (struct list_head*)Add2Ptr( file, usdapi_file_to_list_offset() );
        //
        // Add this inode to internal list to clear later
        //
        spin_lock( &sbi->ddt_lock );
        if ( S_ISDIR( i->i_mode ) ) {
          list_add_tail( lh, &sbi->clear_list );
        } else {
          list_add( lh, &sbi->clear_list );
        }
        DEBUG_ONLY( sbi->nDelClear += 1; )
        TRACE_ONLY( hint = " (d)"; )
        spin_unlock( &sbi->ddt_lock );
      }
    }
  }

#ifdef UFSD_USE_HFS_FORK
  if ( unlikely( is_fork( u ) ) ) {
    UFSD_U( u->fork_inode )->fork_inode = NULL;
    iput( u->fork_inode );
  }
#endif

  VfsTrace( -1, Dbg, ("evict_inode ->%s", hint ) );
}


///////////////////////////////////////////////////////////
// ufsd_show_options
//
// super_operations::show_options
///////////////////////////////////////////////////////////
static int
ufsd_show_options(
    IN struct seq_file  *seq,
#if is_decl( SO_SHOW_OPTIONS_V2 )
    IN struct dentry    *dnt
#else
    IN struct vfsmount  *vfs
#endif
    )
{
#if is_decl( SO_SHOW_OPTIONS_V2 )
  usuper *sbi = UFSD_SB( dnt->d_sb );
#else
  usuper *sbi = UFSD_SB( vfs->mnt_sb );
#endif

  mount_options *opts = &sbi->options;
//  TRACE_ONLY( char *buf = seq->buf + seq->count; )

//  DebugTrace( +1, Dbg, ("show_options: %p", sbi));

#ifdef UFSD_USE_NLS
  {
    int cp;
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      struct nls_table *nls = opts->nls[cp];
      if ( NULL != nls ) {
        seq_printf( seq, ",nls=%s", nls->charset );
      } else {
        seq_printf( seq, ",nls=utf8" );
      }
    }
  }
#endif

  if ( opts->uid )
    seq_printf( seq, ",uid=%d", opts->fs_uid );
  if ( opts->gid )
    seq_printf( seq, ",gid=%d", opts->fs_gid );
  if ( opts->fmask )
    seq_printf( seq, ",fmask=%o", (int)(unsigned short)~opts->fs_fmask );
  if ( opts->dmask )
    seq_printf( seq, ",dmask=%o", (int)(unsigned short)~opts->fs_dmask );
  if ( opts->showmeta )
    seq_printf( seq, ",showmeta" );
  if ( opts->sys_immutable )
    seq_printf( seq, ",sys_immutable" );
  if ( opts->nocase )
    seq_printf( seq, ",nocase" );
  if ( opts->noatime )
    seq_printf( seq, ",noatime" );
  if ( opts->bestcompr )
    seq_printf( seq, ",bestcompr" );
  if ( opts->sparse )
    seq_printf( seq, ",sparse" );
  if ( opts->force )
    seq_printf( seq, ",force" );
  if ( opts->nohidden )
    seq_printf( seq, ",nohidden" );
  if ( opts->acl )
    seq_printf( seq, ",acl" );
#if defined Try_to_writeback_inodes_sb
  if ( opts->wbMb_in_pages )
    seq_printf( seq, ",wb=%uM", opts->wbMb_in_pages >> LOG2OF_PAGES_PER_MB );
#endif
  else if ( opts->wb )
    seq_printf( seq, ",wb=%u", opts->wb );
  if ( opts->raKb ) {
    if ( 0 == (opts->raKb&0x3ff) )
      seq_printf( seq, ",ra=%uM", opts->raKb>>10 );
    else
      seq_printf( seq, ",ra=%u", opts->raKb );
  }
  if ( opts->discard )
    seq_printf( seq, ",discard" );
  if ( UFSD_SAFE_ORDER == opts->safe )
    seq_printf( seq, ",safe=order" );
  else if ( UFSD_SAFE_JNL == opts->safe )
    seq_printf( seq, ",safe=jnl" );
  if ( opts->no_acs_rules )
    seq_printf( seq, ",no_acs_rules" );
  if ( opts->fast_mount )
    seq_printf( seq, ",fast_mount" );

//  DebugTrace( -1, Dbg, ("show_options -> \"%s\"", buf));
  return 0;
}


//
// Volume operations
// super_block::s_op
//
static const struct super_operations ufsd_sops = {
  .alloc_inode    = ufsd_alloc_inode,
  .destroy_inode  = ufsd_destroy_inode,
  .put_super      = ufsd_put_super,
  .statfs         = ufsd_statfs,
  .remount_fs     = ufsd_remount,
  .sync_fs        = ufsd_sync_volume,
  .write_inode    = ufsd_write_inode,
  .evict_inode    = ufsd_evict_inode,
  .show_options   = ufsd_show_options,
};


///////////////////////////////////////////////////////////
// ufsd_get_name
//
// dentry - the directory in which to find a name
// name   - a pointer to a %NAME_MAX+1 char buffer to store the name
// child  - the dentry for the child directory.
//
//
// Get the name of child entry by its ino
// export_operations::get_name
///////////////////////////////////////////////////////////
static int
ufsd_get_name(
    IN struct dentry  *de,
    OUT char          *name,
    IN struct dentry  *ch
    )
{
  int err;
  struct inode *i_p   = de->d_inode;
  struct inode *i_ch  = ch->d_inode;
  usuper *sbi = UFSD_SB( i_ch->i_sb );

  DebugTrace( +1, Dbg, ("get_name: r=%lx=%p('%.*s'), r=%lx=%p('%.*s')",
              i_p->i_ino, de, (int)de->d_name.len, de->d_name.name,
              i_ch->i_ino, ch, (int)ch->d_name.len, ch->d_name.name ));

  //
  // Reset returned value
  //
  name[0] = 0;

  //
  // Call UFSD
  //
  lock_ufsd( sbi );

  err = 0 == ufsdapi_file_get_name( sbi->ufsd, UFSD_FH(i_ch), i_p->i_ino, name, NAME_MAX )
     ? 0
     : -ENOENT;

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("get_name -> %d (%s)", err, name ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_get_parent
//
// export_operations::get_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_get_parent(
    IN struct dentry *de
    )
{
  ufsd_iget5_param param;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  DebugTrace( +1, Dbg, ("get_parent: r=%lx, h=%p, ('%.*s')", i->i_ino, UFSD_FH(i), (int)de->d_name.len, de->d_name.name));

  param.subdir_count = 0;

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 == ufsd_open_by_id( sbi, i )
    && 0 == ufsdapi_file_get_parent( sbi->ufsd, UFSD_FH(i), &param.fh, &param.fi ) ) {

    assert( NULL != param.fh );
    param.Create = NULL;
    i = iget5( i->i_sb, &param );

    // Close ufsd handle if it was not used
    if ( unlikely( NULL != param.fh ) )
      ufsdapi_file_close( sbi->ufsd, param.fh );

    if ( unlikely( NULL == i ) )
      i = ERR_PTR( -ENOMEM );

  } else {
    i = ERR_PTR( -ENOENT ); // No parent for given inode
  }

  unlock_ufsd( sbi );

  // Finally get a dentry for the parent directory and return it.
  // d_obtain_alias accepts NULL and error pointers
  de = d_obtain_alias( i );

  if ( likely( !IS_ERR( de ) ) ) {
    DebugTrace( -1, Dbg, ("get_parent -> ('%.*s'), r=%lx, l=%x",
                (int)de->d_name.len, de->d_name.name, i->i_ino, i->i_nlink ));
  } else {
    DebugTrace( -1, Dbg, ("get_parent -> failed %ld", PTR_ERR( de ) ));
  }
  return de;
}


#if defined UFSD_EXFAT || defined UFSD_FAT
///////////////////////////////////////////////////////////
// ufsd_encode_fh
//
// stores in the file handle fragment 'fh' (using at most 'max_len' bytes)
// information that can be used by 'decode_fh' to recover the file refered
// to by the 'struct dentry* de'
//
// export_operations::encode_fh
///////////////////////////////////////////////////////////
static int
ufsd_encode_fh(
#if is_decl( ENCODE_FH_V1 )
    IN struct dentry  *de,
    IN __u32          *fh,
    IN OUT int        *max_len,
    IN int            connectable
#elif is_decl( ENCODE_FH_V2 )
    IN struct inode   *i,
    IN __u32          *fh,
    IN OUT int        *max_len,
    IN struct inode   *connectable
#endif
    )
{
  int type;
  usuper *sbi;

#if is_decl( ENCODE_FH_V1 )
  struct inode *i = de->d_inode;
  DebugTrace( +1, Dbg, ("encode_fh: r=%lx, ('%.*s'), %x",
              i->i_ino, (int)de->d_name.len, de->d_name.name, *max_len ));
#else
  DebugTrace( +1, Dbg, ("encode_fh: r=%lx, %x", i->i_ino, *max_len ));
#endif

  sbi = UFSD_SB( i->i_sb );

  lock_ufsd( sbi );

  type = 0 != ufsdapi_encode_fh( sbi->ufsd, UFSD_FH(i), fh, max_len )
    ? 0xff
    : 3;

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("encode_fh -> %d,%d", type, *max_len) );

  return type;
}


///////////////////////////////////////////////////////////
// ufsd_decode_fh
//
// Helper function for export (inverse function to ufsd_encode_fh)
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_decode_fh(
    IN struct super_block *sb,
    IN const void   *fh,
    IN unsigned     fh_len,
    IN int          parent,
    IN const char   *func
    )
{
  usuper *sbi     = UFSD_SB( sb );
  ufsd_iget5_param param;
  struct inode *i;
  struct dentry *de;

  DebugTrace( +1, Dbg, ("%s: \"%s\" %d,%d", func, sb->s_id, fh_len, parent ));

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 != ufsdapi_decode_fh( sbi->ufsd, fh, fh_len, parent, &param.fh, &param.fi ) )
    i = ERR_PTR( -ENOENT );
  else {
    param.Create = NULL;
    i = iget5( sb, &param );

    // Close ufsd handle if it was not used
    if ( unlikely( NULL != param.fh ) )
      ufsdapi_file_close( sbi->ufsd, param.fh );

    if ( unlikely( NULL == i ) )
      i = ERR_PTR( -ENOMEM );
  }

  unlock_ufsd( sbi );

  // d_obtain_alias accepts NULL and error pointers
  de = d_obtain_alias( i );

  if ( likely( !IS_ERR( de ) ) ) {
    DebugTrace( -1, Dbg, ("%s: -> ('%.*s'), r=%lx, h=%p l=%x m=%o",
                func, (int)de->d_name.len, de->d_name.name,
                i->i_ino, UFSD_FH(i), i->i_nlink, i->i_mode ));
  } else {
    DebugTrace( -1, Dbg, ("%s: -> failed %ld", func, PTR_ERR( de ) ));
  }

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_dentry
//
// encode_export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_dentry(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  return 3 != fh_type? ERR_PTR( -ENOENT ) : ufsd_decode_fh( sb, fid, fh_len, 0, __func__ );
}


///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_parent
//
// encode_export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  return 3 != fh_type? ERR_PTR( -ENOENT ) : ufsd_decode_fh( sb, fid, fh_len, 1, __func__ );
}


//
// NFS operations.
// super_block::s_export_op
//
static const struct export_operations ufsd_encode_export_op = {
  .encode_fh    = ufsd_encode_fh,
  .get_name     = ufsd_get_name,
  .get_parent   = ufsd_get_parent,
  .fh_to_dentry = ufsd_encode_fh_to_dentry,
  .fh_to_parent = ufsd_encode_fh_to_parent,
};
#endif // #if defined UFSD_EXFAT || defined UFSD_FAT


///////////////////////////////////////////////////////////
// ufsd_nfs_get_inode
//
// Helper function for export
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_nfs_get_inode(
    IN struct super_block *sb,
    IN u64        ino,
    IN u32        gen,
    IN const char *func
    )
{
  struct dentry *de;
  struct inode *i;

  DebugTrace( +1, Dbg, ("%s: \"%s\" r=%llx,%u", func, sb->s_id, ino, gen ));

  // Do fast search first
  i = ilookup( sb, ino );

  if ( NULL == i ) {
    ufsd_iget5_param param;
    usuper *sbi = UFSD_SB( sb );

    //
    // Call UFSD library
    //
    lock_ufsd( sbi );

    if ( 0 != ufsdapi_file_open_by_id( sbi->ufsd, ino, &param.fh, &param.fi ) ) {
      i = ERR_PTR( -ENOENT );
    } else {
      // need to close param.fh if not used
      assert( NULL != param.fh );
      param.Create = NULL;
      i = iget5( sb, &param );

      // Close ufsd handle if it was not used
      if ( unlikely( NULL != param.fh ) )
        ufsdapi_file_close( sbi->ufsd, param.fh );

      if ( NULL == i )
        i = ERR_PTR( -ENOMEM );
    }

    unlock_ufsd( sbi );
  }

  if ( !IS_ERR( i ) && i->i_generation != gen ) {
    // we didn't find the right inode...
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** %s: -> stale (%x != %x)", func, i->i_generation, gen ));
    iput( i );
    i = ERR_PTR( -ESTALE );
  }

  // d_obtain_alias accepts NULL and error pointers
  de = d_obtain_alias( i );

  if ( likely( !IS_ERR( de ) ) ) {
    DebugTrace( -1, Dbg, ("%s: -> ('%.*s'), r=%lx, l=%x m=%o",
                         func, (int)de->d_name.len, de->d_name.name,
                         i->i_ino, i->i_nlink, i->i_mode ));
  } else {
    DebugTrace( -1, Dbg, ("%s: -> failed %ld", func, PTR_ERR( de ) ));
  }

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_fh_to_dentry
//
// export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_dentry(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  return
    fh_len >= 2 && ( FILEID_INO32_GEN == fh_type || FILEID_INO32_GEN_PARENT == fh_type )
    ? ufsd_nfs_get_inode( sb, fid->i32.ino, fid->i32.gen, __func__ )
    : NULL;
}


///////////////////////////////////////////////////////////
// ufsd_fh_to_parent
//
// export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  return
    fh_len > 2 && FILEID_INO32_GEN_PARENT == fh_type
    ? ufsd_nfs_get_inode( sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0, __func__ )
    : NULL;
}


//
// NFS operations.
// super_block::s_export_op
//
static const struct export_operations ufsd_export_op = {
  .get_name     = ufsd_get_name,
  .get_parent   = ufsd_get_parent,
  .fh_to_dentry = ufsd_fh_to_dentry,
  .fh_to_parent = ufsd_fh_to_parent,
};


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// ufsd_add_nls
//
//
///////////////////////////////////////////////////////////
static int
ufsd_add_nls(
    IN OUT mount_options *opts,
    IN struct nls_table  *nls
    )
{
  int cp;
  if ( NULL == nls )
    return -1; // error

  for ( cp = 0; cp < opts->nls_count; cp++ ) {
    if ( 0 == strcmp( opts->nls[cp]->charset, nls->charset ) )
      return 0;
  }

  if ( opts->nls_count >= sizeof(opts->nls)/sizeof(opts->nls[0]) )
    return -1; // error

  opts->nls[opts->nls_count] = nls;
  opts->nls_count += 1;
  return 0; // ok
}
#endif


static const char s_Options[][16] = {
  "nocase",           // 0
  "uid",              // 1
  "gid",              // 2
  "umask",            // 3
  "fmask",            // 4
  "dmask",            // 5
  "trace",            // 6
  "log",              // 7
  "sys_immutable",    // 8
  "quiet",            // 9
  "noatime",          // 10
  "bestcompr",        // 11
  "showmeta",         // 12
  "nobuf",            // 13
  "sparse",           // 14
  "codepage",         // 15
  "nls",              // 16
  "iocharset",        // 17
  "force",            // 18
  "nohidden",         // 19
  "clump",            // 20
  "bias",             // 21
  "user_xattr",       // 22 - not used
  "acl",              // 23
  "chkcnv",           // 24
  "cycle",            // 25
  "delim",            // 26
  "nolazy",           // 27
  "nojnl",            // 28
  "wb",               // 29
  "ra",               // 30
  "discard",          // 31
  "safe",             // 32
  "no_acs_rules",     // 33
  "oemcodepage",      // 34
  "utf8",             // 35
  "fast_mount"        // 36
};


///////////////////////////////////////////////////////////
// ufsd_parse_options_substring
//
// Parse options.
// Helper function for 'ufsd_parse_options'
// It fills mount_options *opts
// Returns NULL if ok
///////////////////////////////////////////////////////////
noinline static const char*
ufsd_parse_options_substring(
    IN mount_options *opts,
    IN char          *options,
    IN int            first_mount
    )
{
  char *t,*v,*delim;
  const char* ret = NULL;
  int i;
  char c;
  unsigned long tmp;
#ifdef UFSD_USE_NLS
  char nls_name[50];
#endif

  while ( ( NULL != ( t = strsep( &options, "," ) ) ) ) {

    // Save current pointer to "=" delimiter
    // It will be used to restore current option
    v = delim = strchr( t, '=' );
    if ( NULL != v )
      *v++ = 0;

    for ( i = 0; i < ARRSIZE(s_Options) && strcmp( t, s_Options[i] ); i++ ) {
      ;
    }

    switch( i ) {
      case 0:   // "nocase"
      case 22:  // "user_xattr": cosmetic - disable "ignore option user_xattr" misunderstanding message
      case 23:  // "acl"
      case 28:  // "nojnl"
      case 31:  // "discard"
      case 33:  // "no_acs_rules"
      case 36:  // "fast_mount"
        // Support both forms: 'nocase' and 'nocase=0/1'
        if ( NULL == v || 0 == v[0] ) {
          c = 1;  // parse short form "nocase"
        } else if ( 0 == v[1] && '0' <= v[0] && v[0] <= '9' ) {
          c = (char)(v[0] - '0'); // parse wide form "nocase=X", where X=0,1,..,9
        } else {
          goto Err;
        }
        switch( i ) {
          case 0:   opts->nocase = c; break;
          case 23:  opts->acl = c; break;
          case 28:  opts->nojnl = c; break;
          case 31:  opts->discard = c; break;
          case 33:  opts->no_acs_rules = c; break;
          case 36:  opts->fast_mount = c; break;
        }
        break;
      case 1:   // "uid"
      case 2:   // "gid"
      case 21:  // "bias"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtol( v, &v, 0 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 1: opts->fs_uid = tmp; opts->uid = 1; break;
        case 2: opts->fs_gid = tmp; opts->gid = 1; break;
        case 21: opts->bias = tmp; break;
        }
        break;
      case 3: // "umask"
      case 4: // "fmask"
      case 5: // "dmask"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = ~simple_strtoul( v, &v, 8 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 3: opts->fs_fmask = opts->fs_dmask = tmp; opts->fmask = opts->dmask = 1; break;
        case 4: opts->fs_fmask = tmp; opts->fmask = 1; break;
        case 5: opts->fs_dmask = tmp; opts->dmask = 1; break;
        }
        break;
//      case 20:  // "clump"
      case 30:  // "ra"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtoul( v, &v, 0 );
        if ( 0 == v[0] || 'K' == v[0] ) {
          ;
        } else if ( 'M' == *v ) {
          tmp *= 1024;
        } else {
          goto Err;
        }
        switch( i ) {
//        case 20: opts->clumpKb = tmp; break;
        case 30: opts->raKb = tmp; break;
        }
        break;
      case 29:
        // Supported forms: 'wb', 'wb=...' and 'wb=..M'
        if ( NULL == v || 0 == v[0] ) {
          // Handling "wb" and "wb="
          opts->wb = 1;
        } else {
          // Handling "wb=..."
          tmp = simple_strtoul( v, &v, 0 );
          if ( v[0] == 'M' ) {
            // Check for overflow. note: tmp is 'unsigned long'
            if ( tmp < (UINT_MAX>>LOG2OF_PAGES_PER_MB) ) {
              opts->wbMb_in_pages = tmp << LOG2OF_PAGES_PER_MB;
            } else {
              goto Err;
            }
          } else {
            if ( 0 != v[0] ) goto Err;
            opts->wb = tmp;
          }
        }
        break;
#ifdef UFSD_TRACE
      case 6: // "trace"
        if ( first_mount ) {
          parse_trace_level( v );
        }
        break;
      case 7: // "log"
        if ( NULL == v ) goto Err;
        strncpy( ufsd_trace_file, v, sizeof(ufsd_trace_file) );
        ufsd_trace_file[sizeof(ufsd_trace_file)-1] = 0;
        ufsd_close_trace( 1 );
        break;
      case 25:  // "cycle"
        parse_cycle_value( v );
        break;
#endif
      case 8: // "sys_immutable"
        if ( NULL != v ) goto Err;
        opts->sys_immutable = 1;
        break;
      case 9: // "quiet"
        break;
      case 10: // "noatime"
        if ( NULL != v ) goto Err;
        opts->noatime = 1;
        break;
      case 11: // "bestcompr"
        if ( NULL != v ) goto Err;
        opts->bestcompr = 1;
        break;
      case 12: // "showmeta"
        if ( NULL != v ) goto Err;
        opts->showmeta = 1;
        break;
      case 13: // "nobuf"
        break;
      case 14: // "sparse"
        if ( NULL != v ) goto Err;
        opts->sparse = 1;
        break;
#ifdef UFSD_USE_NLS
      case 15: // "codepage"
        if ( NULL == v || 0 == v[0] ) goto Err;
        sprintf( nls_name, "cp%d", (int)simple_strtoul( v, &v, 0 ) );
        if ( 0 != v[0] ) goto Err;
        v = nls_name;
        // no break here!!
      case 16: // "nls"
      case 17: // "iocharset"
        // Add this nls into array of codepages
        if ( NULL == v || 0 == v[0] || ufsd_add_nls( opts, load_nls(v) ) )
          goto Err;
        break;
#endif
      case 18: // "force"
        if ( NULL != v ) goto Err;
        opts->force = 1;
        break;
      case 19: // "nohidden"
        if ( NULL != v ) goto Err;
        opts->nohidden = 1;
        break;
      case 24: // "chkcnv"
        if ( NULL != v ) goto Err;
        opts->chkcnv = 1;
        break;
#ifdef UFSD_NTFS
      case 26:  // "delim=':'
        if ( NULL == v || 0 == v[0] ) {
          opts->delim = 0;
        } else if ( 0 == v[1] ) {
          opts->delim = v[0];
        } else {
          goto Err;
        }
        break;
#endif
      case 32:  // "safe": safe={jnl,none,order}
        if ( NULL == v || 0 == strcmp( v, "jnl" ) )
          opts->safe = UFSD_SAFE_JNL;
        else if ( 0 == strcmp( v, "order" ) )
          opts->safe = UFSD_SAFE_ORDER;
        else if ( 0 == strcmp( v, "basic" ) )
          opts->safe = UFSD_SAFE_BASIC;
        else
          goto Err;
        break;

#if defined UFSD_FAT && defined UFSD_USE_NLS
      case 34:  // "oemcodepage"
        if ( NULL == v || 0 == v[0] ) goto Err;
        sprintf( nls_name, "cp%d", (int)simple_strtoul( v, &v, 0 ) );
        DebugTrace(0, Dbg, ("OEM %s supported", nls_name));
        if ( 0 != v[0] ) goto Err;
        v = nls_name;
        opts->nls_oem = load_nls(v);
        if ( NULL == opts->nls_oem )
          goto Err;
        break;
#endif
      case 35: // "utf8", ignore option 'cause utf8 is default
        break;

      case ARRSIZE(s_Options):
Err:
        // Return error options
        ret = t;
        break;

      default:
        printk( KERN_WARNING QUOTED_UFSD_DEVICE": ignore option %s\n", t );

    } // switch ( i )

    // Restore options string
    if ( NULL != delim )
      delim[0] = '=';

    // Restore full string
    if ( NULL != options )
      options[-1] = ',';

    if ( NULL != ret )
      return ret; // error

  } // while ( options )
  return NULL;
}


///////////////////////////////////////////////////////////
// ufsd_parse_options
//
// Parse options.
// Helper function for 'fill_super'
// It fills sbi->options
// Returns NULL if ok
///////////////////////////////////////////////////////////
noinline static const char*
ufsd_parse_options(
    IN usuper *sbi,
    IN char   *options,
    IN int    first_mount
    )
{
  mount_options *opts = &sbi->options;
  const char* ret = NULL;

  assert( 0 == opts->nls_count );
  assert( NULL != current->fs );

  //
  // Setup default options
  //
  opts->fs_uid   = __kuid_val( current_uid() );
  opts->fs_gid   = __kgid_val( current_gid() );
  opts->fs_fmask = opts->fs_dmask = ~current_umask();
  opts->no_acs_rules = 0;
  opts->bias     = -1;
  opts->discard  = 0;
  opts->wbMb_in_pages = 0;
  opts->safe     = UFSD_SAFE_BASIC;

  if ( NULL != options && 0 != options[0] ) {
    ret = ufsd_parse_options_substring( opts, options, first_mount );
    if ( NULL != ret )
      return ret;
  }

#if defined CONFIG_PROC_FS
  mutex_lock( &s_OptionsMutex );
  if ( 0 != ufsd_proc_mount_options[0] ) {
//    ufsd_proc_mount_options[PROC_OPTIONS_MAX_LENGTH-1] = 0; // always set last zero
    ret = ufsd_parse_options_substring( opts, ufsd_proc_mount_options, first_mount );
  }
  mutex_unlock( &s_OptionsMutex );
  if ( NULL != ret )
    return ret;
#endif

#ifdef UFSD_USE_NLS
  //
  // Load default nls if no nls related options
  //
  if ( 0 == opts->nls_count ) {
    struct nls_table *nls_def = load_nls_default();
    if ( NULL != nls_def && 0 != memcmp( nls_def->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
      DebugTrace( 0, Dbg, ("default nls %s", nls_def->charset ));
#endif
      ufsd_add_nls( opts, nls_def );
    }
  } else {
    //
    // Remove kernel utf8 and use builtin utf8
    //
    int cp;
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      struct nls_table *nls = opts->nls[cp];
      if ( 0 == memcmp( nls->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
        DebugTrace( 0, Dbg, ("unload kernel utf8"));
#endif
        unload_nls( nls );
        opts->nls[cp] = NULL;
      } else {
#ifndef UFSD_TRACE_SILENT
        DebugTrace( 0, Dbg, ("loaded nls %s", nls->charset ));
        printk( KERN_NOTICE QUOTED_UFSD_DEVICE": loaded nls %s\n", nls->charset );
#endif
      }
    }
  }
#endif

  //
  // If no nls then use builtin utf8
  //
  if ( 0 == opts->nls_count ) {
#ifndef UFSD_TRACE_SILENT
    DebugTrace( 0, Dbg, ("use builtin utf8" ));
#endif
    opts->nls_count = 1;
    opts->nls[0]    = NULL;
  }

#ifndef CONFIG_FS_POSIX_ACL
  opts->acl = 0;
#endif

#ifdef Try_to_writeback_inodes_sb
  atomic_set( &sbi->writeiter_cnt, opts->wb );
#else
  if ( opts->wb ) {
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"wb=1\" 'cause not supported\n" );
    opts->wb = 0;
  }
#endif

#ifdef Try_to_writeback_inodes_sb
  atomic_set( &sbi->dirty_pages_count, 0 );
#else
  if ( opts->wbMb_in_pages ) {
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"wb=XM\" 'cause not supported\n" );
    opts->wbMb_in_pages = 0;
  }
#endif

  return NULL;
}


///////////////////////////////////////////////////////////
// ufsd_fill_super
//
// This routine is a callback used to recognize and
// initialize superblock using this filesystem driver.
//
// sb - Superblock structure. On entry sb->s_dev is set to device,
//     sb->s_flags contains requested mount mode.
//     On exit this structure should have initialized root directory
//     inode and superblock callbacks.
//
// data - mount options in a string form.
//
// silent - non-zero if no messages should be displayed.
//
// Return: mount error code (0 means success)
///////////////////////////////////////////////////////////
noinline static int
ufsd_fill_super(
    IN OUT struct super_block *sb,
    IN void *data,
    IN int  silent,
    IN int  first_mount
    )
{
  ufsd_volume_info  info;
  ufsd_iget5_param param;
  ufsd_volume* Volume       = NULL;
  int err                   = -EINVAL; // default error
  usuper *sbi               = NULL;
  struct inode *i           = NULL;
  const char *parse_err     = NULL;
  struct block_device *bdev = sb->s_bdev;
  UINT64 sb_size            = bdev->bd_inode->i_size;
//  unsigned int sector_size  = bdev_physical_block_size( bdev );
  unsigned int sector_size  = bdev_logical_block_size( bdev );

  TRACE_ONLY( const char *hint = ""; )

  // before heap alloc
  ufsd_check_sp_start( __func__ );

  sbi = ufsd_heap_alloc( sizeof(usuper), 1 );
  assert(NULL != sbi);
  if ( NULL == sbi )
    return -ENOMEM;

  sbi->sb = sb;

  mutex_init( &sbi->api_mutex );

  spin_lock_init( &sbi->ddt_lock );
  spin_lock_init( &sbi->nocase_lock );
  INIT_LIST_HEAD( &sbi->clear_list );
  INIT_LIST_HEAD( &sbi->write_list );

#if UFSD_SMART_DIRTY_SEC
  rwlock_init( &sbi->state_lock );
  init_waitqueue_head( &sbi->wait_done_flush );
  init_waitqueue_head( &sbi->wait_exit_flush );
#endif

  DEBUG_ONLY( spin_lock_init( &sbi->prof_lock ) );

  //
  // Save current bdi to check for media surprise remove
  // Set it before ufsd_parse_options (it will use sbi->bdi)
  //
  sbi->bdi = sb->s_bdi;

  //
  // Check for size
  //
  if ( sb_size <= 10*PAGE_SIZE ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": \"%s\": the volume size (0x%llx bytes) is too small to keep any fs\n", sb->s_id, sb_size );
    TRACE_ONLY( hint = "too small"; )
    goto ExitInc;
  }

  //
  // Parse options
  //
  parse_err = ufsd_parse_options( sbi, (char*)data, first_mount );
  if ( NULL != parse_err ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount \"%s\". bad option \"%s\"\n", sb->s_id, parse_err );
    TRACE_ONLY( hint = "bad options"; )
    goto ExitInc;
  }

  //
  // Now trace is used
  //
  VfsTrace( +1, Dbg, ("fill_super(\"%s\"), %u: %lx, \"%s\", %s", sb->s_id, jiffies_to_msecs(jiffies-StartJiffies),
                       sb->s_flags, (char*)data,  silent ? "silent" : "verbose"));

#ifdef UFSD_DEBUG
  si_meminfo( &sbi->sys_info );

  DebugTrace( 0, Dbg, ("Pages: total=%lx, free=%lx, buff=%lx, shift="__stringify(PAGE_SHIFT)", stack="__stringify(THREAD_SIZE)"\n",
                        sbi->sys_info.totalram, sbi->sys_info.freeram, sbi->sys_info.bufferram ));
  DebugTrace( 0, Dbg, ("Page flags: lck=%x, err=%x, ref=%x, upt=%x, drt=%x, wrb=%x, priv=%x, swc=%x\n",
                        1u<<PG_locked, 1u<<PG_error, 1u<<PG_referenced, 1u<<PG_uptodate, 1u<<PG_dirty, 1u<<PG_writeback, 1u<<PG_private, 1u<<PG_swapcache ));
  DebugTrace( 0, Dbg, ("Buff flags: upt=%x, drt=%x, lck=%x, map=%x, new=%x, del=%x, aread=%x, awrite=%x",
                        1u<<BH_Uptodate, 1u<<BH_Dirty, 1u<<BH_Lock, 1u<<BH_Mapped, 1u<<BH_New, 1u<<BH_Delay, 1u<<BH_Async_Read, 1u<<BH_Async_Write ));
  DebugTrace( 0, Dbg, ("O_flags: sync=o%o, dsync=o%o, di=o%o, ap=o%o",
                        __O_SYNC, O_DSYNC, O_DIRECT, O_APPEND ));
#endif

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

  sb_set_blocksize( sb, PAGE_SIZE );
  sbi->dev_size   = sb_size;
  sbi->max_block  = sb_size >> PAGE_SHIFT;

  //
  // set s_fs_info to access options in BdRead/BdWrite
  //
  sb->s_fs_info = sbi;

  //
  // 'dev' member of superblock set to device in question.
  // At exit in case of filesystem been
  // successfully recognized next members of superblock should be set:
  // 's_magic'    - filesystem magic nr
  // 's_maxbytes' - maximal file size for this filesystem.
  //
  VfsTrace( 0, Dbg, ("\"%s\": size = 0x%llx*0x%x >= 0x%llx*0x%lx, bs=%x",
                        sb->s_id, sb_size>>blksize_bits( sector_size ), sector_size,
                        sbi->max_block, PAGE_SIZE,
                        bdev_physical_block_size( bdev )));

  {
    struct request_queue * q = bdev_get_queue( bdev );
    if ( NULL == q || !blk_queue_discard( q ) || 0 == q->limits.discard_granularity ) {
      DebugTrace( 0, Dbg, ( "\"%s\": no discard", sb->s_id ));
    } else {
      sbi->discard_granularity          = q->limits.discard_granularity;
      sbi->discard_granularity_mask_inv = ~(UINT64)(sbi->discard_granularity-1);
      SetFlag( sbi->flags, UFSD_SBI_FLAGS_DISRCARD );
      DebugTrace( 0, Dbg, ( "\"%s\": discard_granularity = %x, max_discard_sectors = %x", sb->s_id, sbi->discard_granularity, q->limits.max_discard_sectors ));
    }
  }

  err = ufsdapi_volume_mount( sb, sector_size, &sb_size, &sbi->options, &Volume, PAGE_SIZE, &sbi->fi );

  if ( 0 != err ) {
    if ( !silent )
      printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount \"%s\"\n", sb->s_id);
    TRACE_ONLY( hint = "unknown fs"; )
    err = -EINVAL;
    goto Exit;
  }

#if is_struct( SUPER_BLOCK_S_D_OP )
  sb->s_d_op = sbi->options.use_dop? &ufsd_dop : NULL;
#endif

#if !is_decl( INODE_OPS_XATTR_ANY )
  sb->s_xattr = ufsd_xattr_handlers;
#endif

//  sb->s_flags |= MS_NODIRATIME|MS_NOATIME;
  sb->s_flags = (sb->s_flags & ~MS_POSIXACL) | MS_NODIRATIME
            | (sbi->options.noatime? MS_NOATIME : 0)
            | (sbi->options.acl? MS_POSIXACL : 0);

  //
  // At this point filesystem has been recognized.
  // Let's query for it's capabilities.
  //
  ufsdapi_query_volume_info( Volume, &info, NULL, 0, NULL );

  if ( info.ReadOnly && !FlagOn( sb->s_flags, MS_RDONLY ) ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": No write support. Marking filesystem read-only\n");
    sb->s_flags |= MS_RDONLY;
  }

  if ( !FlagOn( sb->s_flags, MS_RDONLY ) ) {

    if ( ( is_refs( &sbi->options ) || is_refs3( &sbi->options ) ) && THREAD_SIZE < 16*1024 ) {
      printk( KERN_CRIT QUOTED_UFSD_DEVICE": \"%s\": Refs rw requires 16K+ kernel stack (THREAD_SIZE=%uK)\n", sb->s_id, (unsigned)(THREAD_SIZE >> 10) );
      err = -EINVAL;
      goto Exit;
    }

    //
    // Check for dirty flag
    //
    if ( info.dirty && !sbi->options.force ) {
      printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
      TRACE_ONLY( hint = "no \"force\" and dirty"; )
      err = -1000; // Return special value to detect no 'force'
      goto Exit;
    }
  }

#ifdef UFSD_NTFS
  //
  // Allocate helper buffer
  //
  if ( is_ntfs( &sbi->options ) ) {
    sbi->rw_buffer = vmalloc( RW_BUFFER_SIZE );
    if ( NULL == sbi->rw_buffer ) {
      err = -ENOMEM;
      goto Exit;
    }
  }
#endif

  //
  // Set maximum file size and 'end of directory'
  //
  sb->s_maxbytes  = is_ntfs( &sbi->options )? MAX_LFS_FILESIZE : info.maxbytes;
#ifdef UFSD_NTFS
  sbi->maxbytes   = info.maxbytes;
#endif

  sbi->end_of_dir = info.end_of_dir;

  // Always setup 's_time_gran' even you never use it explicitly
  if ( is_hfs( &sbi->options ) )
    sb->s_time_gran = NSEC_PER_SEC; // 1 sec
  else if ( is_fat( &sbi->options ) )
    sb->s_time_gran = 1;  // to avoid mix time manipulation (vfs rounds down, ufsd rounds up) use maximum time resulion
  else if ( is_exfat( &sbi->options ) )
    sb->s_time_gran = NSEC_PER_SEC / 100; // 10 msec
  else
    sb->s_time_gran = 100; // 100 nsec

  sbi->bytes_per_cluster  = info.bytes_per_cluster;
  sbi->cluster_mask       = info.bytes_per_cluster-1;
  sbi->cluster_mask_inv   = ~(UINT64)sbi->cluster_mask;

  //
  // At this point I know enough to allocate my root.
  //
  sb->s_magic       = info.fs_signature;
  sb->s_op          = &ufsd_sops;
  // NFS support
#if defined UFSD_EXFAT || defined UFSD_FAT
  if ( sbi->options.exfat || sbi->options.fat )
    sb->s_export_op = &ufsd_encode_export_op;
  else
#endif
    sb->s_export_op = &ufsd_export_op;

  sbi->ufsd         = Volume;
  assert(UFSD_SB( sb ) == sbi);
  assert(UFSD_VOLUME(sb) == Volume);

  param.subdir_count = 0;
  if ( 0 == ufsdapi_file_open( Volume, NULL, "/", 1, NULL,
#ifdef UFSD_COUNT_CONTAINED
                              &param.subdir_count,
#else
                              NULL,
#endif
                              &param.fh, &param.fi ) ) {
    param.Create = NULL;
    i = iget5( sb, &param );
  }

  if ( unlikely( NULL == i ) ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to open root on \"%s\"\n", sb->s_id );
    TRACE_ONLY( hint = "open root"; )
    err = -EINVAL;
    goto Exit;
  }

  // Always clear S_IMMUTABLE
  i->i_flags &= ~S_IMMUTABLE;
#if is_decl( D_MAKE_ROOT )
  sb->s_root = d_make_root( i );
#else
  sb->s_root = d_alloc_root( i );
#endif

  if ( NULL == sb->s_root ) {
    iput( i );
    TRACE_ONLY( hint = "no memory"; )
    err = -ENOMEM;
    // Not necessary to close root_ufsd
    goto Exit;
  }

  // Create /proc/fs/ufsd/..
  ufsd_proc_info_create( sb );

  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_SHIFT-10 );

  //
  // Start flush thread.
  // To simplify remount logic do it for read-only volumes too
  //
#if UFSD_SMART_DIRTY_SEC
  {
    void *p = kthread_run( ufsd_flush_thread, sb, "ufsd_%s", sb->s_id );
    if ( IS_ERR( p ) ) {
      err = PTR_ERR( p );
      goto Exit;
    }

    wait_event( sbi->wait_done_flush, NULL != sbi->flush_task );
  }
#endif

  //
  // Done.
  //
  VfsTrace( -1, Dbg, ("fill_super(\"%s\"), %u -> sb=%p,i=%p,r=%lx,uid=%d,gid=%d,m=%o", sb->s_id, jiffies_to_msecs(jiffies-StartJiffies), sb, i,
                        i->i_ino, __kuid_val( i->i_uid ), __kgid_val( i->i_gid ), i->i_mode ));

  err = 0;

  if ( 0 ) {
ExitInc:
#ifdef UFSD_TRACE
    if ( ufsd_trace_level & Dbg )
      ufsd_trace_inc( +1 ); // compensate the last 'DebugTrace( -1, ... )'
#endif

Exit:
    //
    // Free resources allocated in this function
    //
    if ( NULL != Volume )
      ufsdapi_volume_umount( Volume );

    assert( NULL != sbi );
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
    mutex_destroy( &sbi->api_mutex );
#endif
    ufsd_uload_nls( &sbi->options );

    VfsTrace( -1, Dbg, ("fill_super failed to mount %s: \"%s\" ->%d", sb->s_id, hint, err));

    ufsd_heap_free( sbi );
    sb->s_fs_info = NULL;
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_fill_super_trace
//
// This is a "pass thru" wrapper call for `ufsd_fill_super' callback
// to try mount again when mount operation was failed
// and trace level was less than "all"
//
///////////////////////////////////////////////////////////
static inline int
ufsd_fill_super_trace(
    IN OUT struct super_block *sb,
    IN void *data,
    IN int  silent
    )
{
  int err = ufsd_fill_super( sb, data, silent, 1 );

#ifdef UFSD_TRACE
  if ( unlikely( 0 != err ) ) {
    mutex_lock( &s_MountMutex );
    if ( UFSD_LEVEL_DEFAULT == ufsd_trace_level ) {
      int ret;
      ufsd_trace_level = UFSD_LEVEL_STR_ALL;

      ret = ufsd_fill_super( sb, data, silent, 0 );

      ufsd_trace_level = UFSD_LEVEL_DEFAULT;
      assert( 0 != ret && ret == err );
      err = ret;
    }
    mutex_unlock( &s_MountMutex );
  }
#endif

  return err;
}


#if is_struct( FILE_SYSTEM_TYPE_MOUNT )
///////////////////////////////////////////////////////////
// ufsd_mount (2.6.38+)
//
// fstype::mount
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_mount(
    IN struct file_system_type  *fs_type,
    IN int        flags,
    IN const char *dev_name,
    IN void       *data
    )
{
  return mount_bdev( fs_type, flags, dev_name, data, ufsd_fill_super_trace );
}

#else

///////////////////////////////////////////////////////////
// ufsd_get_sb  [2.6.18 - 2.6.38]
//
// fstype::get_sb
///////////////////////////////////////////////////////////
static int
ufsd_get_sb(
    IN struct file_system_type  *fs_type,
    IN int              flags,
    IN const char       *dev_name,
    IN void             *data,
    IN struct vfsmount  *mnt
    )
{
  int err = get_sb_bdev( fs_type, flags, dev_name, data, ufsd_fill_super_trace, mnt );
  if ( 0 == err ) {
    // Save mount path to correct ntfs symlinks (see ufsd_readlink_hlp)
    usuper *sbi = UFSD_SB( mnt->mnt_sb );
    sbi->vfs_mnt = mnt;
  }
  return err;
}
#endif

static struct file_system_type ufsd_fs_type = {
  .name       = QUOTED_UFSD_DEVICE,
  .fs_flags   = FS_REQUIRES_DEV,
#if is_struct( FILE_SYSTEM_TYPE_MOUNT )
  .mount      = ufsd_mount,
#else
  .get_sb     = ufsd_get_sb,
#endif
  .kill_sb    = kill_block_super,
  .owner      = THIS_MODULE,
};


#if 0
///////////////////////////////////////////////////////////
// kconfig_check_inode
//
// runtime check config match
///////////////////////////////////////////////////////////
static inline int
kconfig_check_inode( void )
{
  int ret = 1; // assume fail
  const size_t isize  = sizeof( struct inode );
  unsigned char *ibuf = kmalloc( isize * 4, GFP_KERNEL );

  if ( NULL != ibuf ) {
    struct super_block *sb = kmalloc( sizeof( struct super_block ), GFP_KERNEL );
    if ( NULL != sb ) {
      struct inode *i = ibuf;
      memset( i,  0xFF, isize * 4 );
      memset( sb, 0xEE, sizeof( struct super_block ) );
      if ( 0 == inode_init_always( sb, i )
        && 0 == i->i_private && 0 == ibuf[isize-1] && 0xFF == ibuf[isize] ) {
        ret = 0;  // ok
      }
      kfree( sb );
    }
    kfree( ibuf );
  }

  return ret;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_kconfig_check
//
// kernel config check function
///////////////////////////////////////////////////////////
static inline int
ufsd_kconfig_check(void)
{
#ifdef KMALLOC_CACHES
  // struct kmem_cache size kernel config check function
  // This check is actual for <= 2.6.36 only:
  //  - http://lxr.free-electrons.com/source/include/linux/slub_def.h?v=2.6.36#L154
  {
    void *p = kmalloc( PAGE_SIZE, GFP_KERNEL );
    if ( NULL != p )
      kfree( p );
  }
#endif
  return 0; //kconfig_check_inode();
}


///////////////////////////////////////////////////////////
// ufsd_init
//
// module init function
///////////////////////////////////////////////////////////
static int
__init ufsd_init(void)
{
  int ret;
  int EndianError;

  TRACE_ONLY( mutex_init( &s_MountMutex ); )
  TRACE_ONLY( StartJiffies=jiffies; )

  TRACE_ONLY( parse_trace_level( ufsd_trace_level_ ); )

  ufsd_check_sp_start( __func__ );

#ifdef UFSD_TRACE_SILENT
  ufsdapi_library_version( &EndianError );
#else
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver (%s) loaded at %p\n%s%s", s_DriverVer, UFSD_MODULE_CORE(), ufsdapi_library_version( &EndianError ),
#ifdef DEFAULT_MOUNT_OPTIONS
      "Default options: " DEFAULT_MOUNT_OPTIONS "\n"
#endif
      "" );
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": PAGE_SIZE=%uK, THREAD_SIZE=%uk\n", (unsigned)(PAGE_SIZE>>10), (unsigned)(THREAD_SIZE>>10) );
#endif

  if ( EndianError )
    return -EINVAL;

  if ( ufsd_kconfig_check() ) {
    printk( KERN_CRIT QUOTED_UFSD_DEVICE": probable kernel config mismatch" );
    return -ENOEXEC;
  }

#ifdef UFSD_HASH_VAL_H
  ufsd_check_config_hash();
#endif

#if defined UFSD_EXFAT || defined UFSD_FAT
  //
  // exfat stores dates relative 1980
  // 'get_seconds' returns seconds since 1970
  // Check current date
  if ( get_seconds() < Seconds1970To1980 )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": (ex)fat can't store dates before Jan 1, 1980. Please update current date\n" );
#endif

  ufsd_proc_create();

  unode_cachep  = kmem_cache_create( QUOTED_UFSD_DEVICE "_unode_cache", sizeof(unode), 0, SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD, init_once );
  if ( NULL == unode_cachep ) {
    ret = -ENOMEM;
  } else {
    //
    // Allow UFSD to init globals
    //
    ret = ufsdapi_main( 0 );
    if ( 0 == ret ) {
      //
      // Finally register filesystem
      //
      ret = register_filesystem( &ufsd_fs_type );
      if ( 0 == ret )
        return 0; // Ok
    }

    //
    // Deinit UFSD globals
    //
    ufsdapi_main( 1 );

    kmem_cache_destroy( unode_cachep );
  }

  // remove /proc/fs/ufsd
  ufsd_proc_delete();

//  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ufsd_init failed %d\n", ret );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_exit
//
// module exit function
///////////////////////////////////////////////////////////
static void
__exit ufsd_exit(void)
{
#ifdef UFSD_DEBUG_ALLOC
  struct list_head *pos, *pos2;
#endif
  // remove /proc/fs/ufsd
  ufsd_proc_delete();

  unregister_filesystem( &ufsd_fs_type );

  ufsd_check_sp_start( __func__ );

  //
  // Deinit UFSD globals
  //
  ufsdapi_main( 1 );

  kmem_cache_destroy( unode_cachep );

#ifndef UFSD_TRACE_SILENT
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver unloaded\n" );
#endif

#if defined UFSD_TRACE && defined UFSD_DEBUG && defined UFSD_USED_SHARED_FUNCS
  {
    int i;
    for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
      if ( 0 != s_shared[i].cnt ) {
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("forgotten shared ptr %p,%x,%d", s_shared[i].ptr, s_shared[i].len, s_shared[i].cnt ));
      }
    }
  }
#endif

#ifdef UFSD_DEBUG_ALLOC
  assert(0 == TotalAllocs);
  trace_mem_report( 1 );
  list_for_each_safe( pos, pos2, &TotalAllocHead )
  {
    memblock_head *block = list_entry( pos, memblock_head, Link );
    unsigned char *p = (unsigned char*)(block+1);
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("block %p, seq=%x, 0x%x bytes: [%*ph]", p, block->seq, block->size, min_t( unsigned, 16, block->size ), p ));

    // Don't: (( block->asize & 1U)? vfree : kfree)( block );
    // 'cause declaration of vfree and kfree differs!
    if ( block->asize & 1U ) {
      vfree( block );
    } else {
      kfree( block );
    }
  }
#ifdef UFSD_DEBUG
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("inuse = %u msec, wait = %u msec, HZ=%u", jiffies_to_msecs( jiffies - StartJiffies ), jiffies_to_msecs( WaitMutex ), (unsigned)HZ ));
#endif
#endif

#ifdef UFSD_CHECK_STACK
  ufsd_printk( NULL, "max stack: %s, %zd", s_max_hint, s_maxdsp );
#endif

  ufsd_close_trace( 0 );
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  TRACE_ONLY( mutex_destroy( &s_MountMutex ); )
#endif
}

//
// And now the modules code and kernel interface.
//
MODULE_DESCRIPTION("Paragon " QUOTED_UFSD_DEVICE " driver");
MODULE_AUTHOR("Andrey Shedel & Alexander Mamaev");
MODULE_LICENSE("Commercial product");

#ifdef UFSD_TRACE
module_param_string(trace, ufsd_trace_level_, sizeof(ufsd_trace_level_), S_IRUGO);
MODULE_PARM_DESC(trace, " trace level for ufsd module");
module_param_string(log, ufsd_trace_file, sizeof(ufsd_trace_file), S_IRUGO);
MODULE_PARM_DESC(log, " ufsd log file, default is system log");
module_param_named(cycle, ufsd_cycle_mb, ulong, S_IRUGO);
MODULE_PARM_DESC(cycle, " the size of cycle log in MB, default is 0");
#endif

module_init(ufsd_init)
module_exit(ufsd_exit)

