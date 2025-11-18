# CLAUDE.md - AI Assistant Guide for Xtank

## Table of Contents
1. [Project Overview](#project-overview)
2. [Codebase Structure](#codebase-structure)
3. [Build System](#build-system)
4. [Coding Conventions](#coding-conventions)
5. [Key Subsystems](#key-subsystems)
6. [Development Workflow](#development-workflow)
7. [Common Patterns](#common-patterns)
8. [Testing](#testing)
9. [Important Files Reference](#important-files-reference)
10. [Gotchas and Important Notes](#gotchas-and-important-notes)

---

## Project Overview

**Xtank** is a multi-player vehicle combat game written in C for X11. Players control tanks equipped with weapons and armor, competing in various game modes within maze-based arenas.

- **Language**: C (C89/C99 compatible)
- **Graphics**: X11 (libX11)
- **License**: BSD 2-clause
- **Primary Platforms**: FreeBSD, Linux, macOS
- **Codebase Size**: ~26,000 lines in Src/, ~5,000 lines in headers
- **Repository Age**: Mature, stable project with ongoing maintenance

### Key Features
- Multi-player networked gameplay
- Robot AI programs (autonomous tank control)
- Thread-based concurrency (one thread per robot)
- Dynamic loading of robot programs
- Vehicle/maze design systems
- Multiple game modes (combat, war, capture the flag, race, etc.)

---

## Codebase Structure

```
xtank/
├── Src/               # Main source code (~45 C files, 26K lines)
│   ├── Include/       # Header files (45 headers, 5K+ lines)
│   ├── Display/       # Graphics rendering (X11 implementation)
│   │   └── Objects/   # Pre-compiled graphical objects (.obj files)
│   ├── Programs/      # Built-in robot AI programs
│   └── Threads/       # Threading implementation
├── Programs/          # External robot programs (dynamically loadable)
├── Mazes/             # Maze definition files (.m files)
├── Vehicles/          # Vehicle configuration files (.V files)
├── Mazes.PIX/         # Pre-compiled maze binaries
├── Vehicles.PIX/      # Pre-compiled vehicle binaries
├── Doc/               # Documentation (LaTeX, README files)
├── Help/              # In-game help system files
├── Bin/               # Build and utility scripts
├── Util/              # Utility programs (mostly untested)
└── Todo/              # Historical TODO lists and project ideas
```

### Key Source Files by Subsystem

**Core Game Engine:**
- `main.c` - Entry point
- `init.c` - Initialization
- `animate.c` - Main game loop and frame timing
- `update.c` - Vehicle/bullet/explosion updates
- `game.c` - Game rules and win conditions

**Vehicle System:**
- `vehicle.c` - Vehicle management
- `actions.c` - Vehicle actions
- `collision.c` - Collision detection

**Graphics/Display:**
- `display.c` - Display management
- `Display/x11.c` - X11 rendering (~43KB)
- `Display/graphics.c` - Graphics primitives
- `Display/three-d.c` - 3D perspective rendering

**UI Components:**
- `menu.c` - Game setup menus
- `newconsole.c` - In-game console
- `hud.c` - Heads-up display
- `radar.c` / `newradar.c` - Radar systems
- `mapper.c` - Map display

**World/Physics:**
- `maze.c` - Maze management
- `box.c` - Grid-based collision system
- `intersect.c` - Intersection calculations

**Weapons/Combat:**
- `hit.c` - Damage calculations
- `explosion.c` - Explosion effects

**Robot AI:**
- `program.c` - Robot program management
- `interface.c` - Robot library API implementation
- `Threads/thread.c` - Thread management

**File I/O:**
- `file.c` - General file operations
- `vdesign.c` - Vehicle design/loading
- `mdesign.c` - Maze design/loading

---

## Build System

### Build Files
- **Makefile** - BSD make (uses bsd.prog.mk)
- **GNUmakefile** - GNU make (Linux, macOS)
- **Src/Makefile.inc1** - Common source files and flags
- **Src/Makefile.inc2** - LEX file generation

### Build Commands
```bash
# Standard build (auto-detects BSD vs GNU make)
make

# Clean build
make clean && make

# Generate cscope database for code navigation
make cscope
```

### Build Configuration

**Required Dependencies:**
- X11 development libraries (`libX11-dev` or `libX11-devel`)
- C compiler (GCC or compatible)
- LEX (flex)
- Standard math library

**Key Compiler Defines:**
- `DX11` - Use X11 graphics (required)
- `DUNIX` - Unix/POSIX OS (required)
- `DS1024x864` - Screen resolution
- `DTHREAD_SWAPCONTEXT` - Thread implementation (most common)
- `DDYNALOAD` - Enable dynamic robot program loading
- `DMULTI_SYNC` - Multi-display synchronization (recommended)
- `DSTINGY_REDRAW` - Smart redrawing (performance optimization)
- `DBATCH_LINES` - Batch line drawing (recommended)
- `DBATCH_POINTS` - Batch point drawing (recommended)
- `DBATCH_COLOR_LINES` - Batch colored lines (recommended)
- `DKEYPAD_DETECT` - Enable keypad input

**Thread Implementation Options (choose ONE):**
- `DTHREAD_SWAPCONTEXT` - Standard Unix getcontext/swapcontext (recommended)
- `DTHREAD_POSIX` - POSIX threads (pthreads)
- `DTHREAD_SUNLWP` - Sun LWP (legacy, unsupported)
- `DTHREAD_NULL` - No threading (robots disabled)

See `Doc/README.defines` for comprehensive build option documentation.

### Running the Game

```bash
# Set XTANK_DIR environment variable
XTANK_DIR=$(pwd) ./Src/xtank

# Or set it permanently in your shell profile
export XTANK_DIR=/path/to/xtank
./Src/xtank
```

---

## Coding Conventions

### Code Formatting

**Indentation Style** (configured in `Src/.indent.pro`):
- 4-space indentation (`-i4`)
- 2-space continuation indent (`-ci2`)
- Tab size 4 (`-ts4`)
- Break before operators (`-br`)
- Space after cast operator (`-ss`)
- Blank lines after declarations (`-bad`)
- Blank lines after procedures (`-bap`)

**To format code:**
```bash
cd Src
indent -v file.c
```

### Naming Conventions

| Type | Convention | Example |
|------|-----------|---------|
| Macros/Constants | UPPERCASE_WITH_UNDERSCORES | `MAX_VEHICLES`, `BOX_WIDTH` |
| Type Names | CamelCase | `Vehicle`, `Bullet`, `Weapon_stat` |
| Enum Values | CamelCase or CAPS_WITH_UNDERSCORES | `COMBAT_GAME`, `MAP_NONE` |
| Functions | lowercase_with_underscores | `make_turrets()`, `fire_weapon()` |
| Variables | lowercase_with_underscores | `turret_weight`, `num_vehicles` |
| Struct typedefs | No "struct" prefix after typedef | `Vehicle v;` not `struct Vehicle v;` |

### File Organization

**Header Guards:**
```c
#ifndef _FILENAME_H_
#define _FILENAME_H_

/* Header content */

#endif /* _FILENAME_H_ */
```

**File Header (BSD License):**
```c
/*-
 * Copyright (c) YYYY Author Name
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * [... full BSD 2-clause license text ...]
 */
```

**Function Prototypes (K&R Compatibility):**
```c
/* In headers */
#if 1 || defined(__STDC__) || defined(__cplusplus)
#define P_(s) s
#else
#define P_(s) ()
#endif

extern void my_function P_((int arg1, char *arg2));
```

**Function Definitions:**
```c
/*
** Brief description of what the function does.
** Can span multiple lines if needed.
*/
static void
function_name(int arg1, char *arg2)
{
    /* Implementation */
}
```

### Common Macros

```c
/* Bit flag manipulation */
#define setflag(var,flag) ((var) |= (flag))
#define clrflag(var,flag) ((var) &= ~(flag))
#define tstflag(var,flag) ((var) & (flag))

/* Math utilities */
#define SQR(x) ((x)*(x))
#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))
#define ABS(x) (((x) < 0) ? -(x) : (x))
```

### Code Style Guidelines

1. **Use descriptive variable names** - Prefer `vehicle_speed` over `vs`
2. **Keep functions focused** - One function, one purpose
3. **Comment complex algorithms** - Explain "why", not "what"
4. **Use status flags consistently** - Via setflag/clrflag/tstflag macros
5. **Avoid magic numbers** - Use named constants
6. **Check return values** - Especially for malloc(), file I/O
7. **Initialize variables** - Don't rely on zero-initialization

---

## Key Subsystems

### 1. Vehicle System

**Core Data Structure:** `Vehicle` (defined in `Include/vehicle.h`)

Key components:
- Position and orientation (`Loc *loc`, `Loc *old_loc`)
- Weapons array (up to `MAX_WEAPONS`)
- Turrets array (up to `MAX_TURRETS`)
- Armor (front, back, left, right, top, bottom)
- Fuel, heat, ammo
- Status flags (`status` field)
- Robot program (`Program *program`)

**Status Flags:**
```c
VS_functioning   // Vehicle is operational
VS_is_alive      // Vehicle has not been destroyed
VS_sliding       // Vehicle is sliding
VS_disc_spin     // Disc vehicle is spinning
VS_moved         // Vehicle moved this frame (STINGY_REDRAW)
VS_rotated       // Vehicle rotated this frame (STINGY_REDRAW)
```

**Vehicle Arrays:**
- `actual_vehicles[]` - All vehicles in game
- `live_vehicles[]` - Pointers to active vehicles
- `dead_vehicles[]` - Pointers to destroyed vehicles
- `num_veh_alive` - Count of active vehicles

### 2. Game Loop

**Main Loop** (`animate.c:animate()`):
```c
while (game_running) {
    1. Check frame timing
    2. Process input events
    3. Update all vehicles (movement, heat, fuel, weapons)
    4. Update all bullets (position, lifetime)
    5. Check collisions (bullets-walls, bullets-vehicles, vehicles-vehicles)
    6. Apply game rules (scoring, win conditions)
    7. Update explosions
    8. Render display
    9. Swap thread contexts (run robot programs)
    10. Sleep until next frame
}
```

### 3. Collision Detection

**Subsystems** (`collision.c`):
- `coll_bullets_maze()` - Bullets hitting walls
- `coll_bullets_vehicles()` - Bullets hitting vehicles
- `coll_vehicles_vehicles()` - Vehicle-to-vehicle collisions
- `coll_vehicle_walls()` - Vehicles hitting walls

**Approach:**
- Bounding box checks
- Grid-based spatial partitioning (maze boxes)
- Line segment intersection tests

### 4. Maze System

**Grid Structure:**
- Maze is `GRID_WIDTH` x `GRID_HEIGHT` (typically 26x26)
- Each cell is a `Box` (defined in `Include/box.h`)
- Boxes contain walls on 4 sides (NORTH, SOUTH, EAST, WEST)
- Walls have types: normal, destructible, indestructible

**Landmarks** (special tiles):
- `FUEL` - Refuel station
- `AMMO` - Ammunition
- `ARMOR` - Repair armor
- `GOAL` - Objective location
- `OUTPOST` - Team base
- `PEACE` - No-combat zone
- `TELE` - Teleporter
- `SCROLL` - Movement modifier
- `SLIP` - Slippery surface
- `SLOW` - Slow surface

### 5. Robot AI System

**Architecture:**
- Each robot runs in its own thread
- Robots call functions from `xtanklib.h` API
- Thread switching occurs each game frame
- Stack size configurable via `STACK_SIZE` define

**Robot API** (selection from `Include/xtanklib.h`):
```c
/* Sensing */
void get_location(Location *loc);
FLOAT speed(void);
int num_vehicles(void);
int get_vehicles(Vehicle_info *info, int max);
Box (*map_get())[GRID_HEIGHT];

/* Control */
void turn_vehicle(Angle desired_heading);
void set_abs_drive(FLOAT speed);
void set_rel_drive(FLOAT speed);

/* Combat */
int fire_weapon(WeaponNum num);
void aim_weapon(WeaponNum num, Angle angle);
void turn_on_weapon(WeaponNum num);
void turn_off_weapon(WeaponNum num);

/* Communication */
void send_msg(ID recipient, Opcode opcode, Byte *data, int length);
int messages(void);
void receive_msg(Message *msg);
```

**Built-in Robot Programs** (in `Programs/`):
- `drone.c` - Simple wandering attacker
- `gnat.c` - Complex tactical AI (largest, 142KB)
- `tagman.c` - Tag game specialist
- `mmtf.c` - Multi-mode tactical fighter
- `shooter.c` - Sharpshooter
- `rdfbot.c` - Radio direction finder user
- And more...

### 6. Graphics System

**Display Architecture:**
- X11-based rendering
- Multiple terminal support (split-screen, networked)
- 2D overhead view (primary)
- 3D perspective view (optional)

**Key Files:**
- `Display/x11.c` - X11 window management, drawing primitives
- `Display/graphics.c` - High-level graphics functions
- `Display/XMultiSync.c` - Multi-display synchronization
- `Display/three-d.c` - 3D rendering

**Optimization Techniques:**
- Batched drawing (`BATCH_LINES`, `BATCH_POINTS`, `BATCH_COLOR_LINES`)
- Smart redrawing (`STINGY_REDRAW` - only redraw moved objects)
- Pre-compiled graphical objects (.obj files)

**Object Files** (`Display/Objects/`):
- Vehicle shapes (`.obj` format)
- Bullet/explosion sprites
- Landmark graphics

### 7. Weapon System

**Weapon Properties** (from `Include/weapon-defs.h`):
- Damage, reload time, range, speed
- Ammo consumption
- Mount restrictions (turret, side-specific)
- Special flags:
  - `F_BEAM` - Laser/beam weapon
  - `F_MINE` - Mine behavior
  - `F_SLICK` - Leaves oil slick
  - `F_AREA` - Area effect
  - `F_KEYB` - Player-controlled
  - `F_TELE` - Can teleport with vehicle

**Bullet Lifecycle:**
- Creation at weapon mount point
- Update position each frame
- Check collisions
- Explode on impact or timeout
- Transition to explosion effect

---

## Development Workflow

### Making Changes

1. **Before Starting:**
   - Read relevant documentation in `Doc/`
   - Check `Todo/` for known issues
   - Review recent commits for context

2. **During Development:**
   - Follow coding conventions (see above)
   - Test on target platform(s)
   - Watch for compiler warnings (project aims for zero warnings)
   - Use `indent` to format code

3. **Testing:**
   - Build and run the game
   - Test with multiple vehicles
   - Test relevant game modes
   - Check for memory leaks (consider `libefence`)

4. **Committing:**
   - Write clear commit messages
   - Reference issue numbers if applicable
   - Keep commits focused and atomic

### Current Development Focus

Based on recent commits:
- **Reducing compiler warnings** (high priority)
- **Code cleanup and refactoring**
- **Enum and type safety improvements**
- **Cross-platform compatibility**

### Adding New Features

**New Robot Program:**
1. Create `Programs/myrobot.c`
2. Implement required functions: `myrobot_main()`, `myrobot_prog`
3. Add to `Src/Makefile.inc1` in `PROG_SRCS`
4. Rebuild

**New Weapon:**
1. Add definition to `Include/weapon-defs.h`
2. Update weapon statistics arrays
3. Add graphics if needed
4. Implement special behavior in `hit.c`, `update.c`

**New Game Mode:**
1. Add enum value to `Game` type (`Include/game.h`)
2. Implement rules in `game.c`
3. Add menu entry in `menu.c`
4. Update documentation

### Debugging Tips

**Enable Debug Output:**
- Add `-DDEBUG` to compiler flags
- Check `unix.c` for debug output

**Memory Debugging:**
- Link with `libefence` (Electric Fence)
- Use valgrind on Linux

**GDB Usage:**
- Game is compiled with `-g` by default
- Set `XTANK_DIR` before running in GDB:
  ```bash
  gdb ./Src/xtank
  (gdb) set environment XTANK_DIR /path/to/xtank
  (gdb) run
  ```

**Common Issues:**
- **Segfault in robot program**: Check stack size (`STACK_SIZE`)
- **Display corruption**: Check `STINGY_REDRAW` logic
- **Collision bugs**: Verify bounding box calculations
- **Threading issues**: Check thread implementation (`THREAD_*` define)

---

## Common Patterns

### Status Flag Pattern

**Usage:**
```c
/* Setting flags */
setflag(v->status, VS_functioning);
setflag(v->status, VS_is_alive);

/* Clearing flags */
clrflag(v->status, VS_sliding);

/* Testing flags */
if (tstflag(v->status, VS_is_alive)) {
    /* Vehicle is alive */
}

/* Multiple flags */
if (tstflag(v->status, VS_functioning | VS_is_alive)) {
    /* At least one flag is set */
}

if ((v->status & (VS_functioning | VS_is_alive)) == (VS_functioning | VS_is_alive)) {
    /* Both flags are set */
}
```

### Enumeration with MAX Pattern

**Definition:**
```c
typedef enum {
    COMBAT_GAME,
    WAR_GAME,
    ULTIMATE_GAME,
    CAPTURE_GAME,
    RACE_GAME,
    MAX_GAMES  /* Always last - used for array sizing */
} Game;
```

**Usage:**
```c
/* Array sized by enum */
char *game_names[MAX_GAMES] = {
    "Combat",
    "War",
    "Ultimate",
    "Capture",
    "Race"
};

/* Iteration */
for (i = 0; i < MAX_GAMES; i++) {
    printf("%s\n", game_names[i]);
}
```

### Location Tracking Pattern

**Double-buffering for position:**
```c
typedef struct {
    Loc *loc;      /* Current location */
    Loc *old_loc;  /* Previous location */
    Loc loc1;      /* Storage buffer 1 */
    Loc loc2;      /* Storage buffer 2 */
} Vehicle;

/* Swap buffers */
temp = v->loc;
v->loc = v->old_loc;
v->old_loc = temp;

/* Now update v->loc with new position */
/* old_loc automatically has previous frame's position */
```

### Bullet/Object Set Pattern

**Definition:**
```c
typedef struct {
    Bullet *list[MAX_BULLETS];  /* Array of pointers */
    int number;                 /* Count of active bullets */
} Bset;
```

**Usage:**
```c
/* Adding a bullet */
if (bset->number < MAX_BULLETS) {
    bset->list[bset->number++] = new_bullet;
}

/* Iterating */
for (i = 0; i < bset->number; i++) {
    bullet = bset->list[i];
    /* Process bullet */
}

/* Removing a bullet (swap with last, decrement count) */
bset->list[i] = bset->list[--bset->number];
```

### Terminal Iteration Pattern

**Global terminal array:**
```c
extern Terminal *terminal[];
extern int num_terminals;

/* Iterate over all displays */
for (i = 0; i < num_terminals; i++) {
    term = terminal[i];
    /* Render to this terminal */
}
```

---

## Testing

### Test Resources

**Test Mazes:**
- `Mazes/TestMaze.m`
- `Mazes/TestMaze2.m`
- `Mazes/TestMaze3.m`
- `Mazes/TestMaze4.m`
- `Mazes/TestRace.m`

**Test Vehicles:**
- `Vehicles/Test.V`
- `Vehicles/test.V`
- `Vehicles/test2.V`

### Testing Approach

**Manual Testing Checklist:**
1. Game launches without errors
2. Menus are responsive
3. Maze loads correctly
4. Vehicles spawn in correct positions
5. Vehicle movement works
6. Weapons fire correctly
7. Collisions are detected
8. Damage is calculated properly
9. Game rules enforced
10. No memory leaks (use valgrind)
11. No segfaults
12. Graceful exit

**Multi-player Testing:**
1. Launch game on multiple displays
2. Verify synchronization
3. Test network performance
4. Verify team mechanics
5. Test message passing

**Robot Program Testing:**
1. Load robot program
2. Verify thread creation
3. Monitor robot behavior
4. Check for infinite loops
5. Verify API calls work correctly

**Note:** No formal unit test framework exists. Testing is primarily integration/manual testing through gameplay.

---

## Important Files Reference

### Essential Headers

| File | Purpose |
|------|---------|
| `Include/xtank.h` | Main header, global state |
| `Include/vehicle.h` | Vehicle structure and constants |
| `Include/bullet.h` | Bullet/explosion structures |
| `Include/weapon.h` | Weapon system types |
| `Include/weapon-defs.h` | Weapon definitions and statistics |
| `Include/maze.h` | Maze structures |
| `Include/box.h` | Grid box structures |
| `Include/game.h` | Game types and rules |
| `Include/terminal.h` | Display/terminal structures |
| `Include/thread.h` | Threading abstractions |
| `Include/xtanklib.h` | Robot program API |
| `Include/special.h` | Special equipment types |
| `Include/message.h` | Message passing |

### Key Documentation

| File | Purpose |
|------|---------|
| `README.md` | Project overview, build instructions |
| `Doc/README.defines` | Comprehensive build options guide |
| `Doc/Src/xtanklib.latex` | Robot API documentation |
| `Doc/Src/vdesign.latex` | Vehicle design system |
| `Doc/Src/mdesign.latex` | Maze design system |
| `Src/Xtank.man` | Manual page |
| `aaron_notes` | Historical design notes |
| `mark.bullen_readme` | Developer notes |

### Configuration Files

| File | Purpose |
|------|---------|
| `Makefile` | BSD make configuration |
| `GNUmakefile` | GNU make configuration |
| `Src/.indent.pro` | Code formatting rules |
| `Src/Makefile.inc1` | Source files and compiler flags |
| `Src/Makefile.inc2` | Generated file rules |

---

## Gotchas and Important Notes

### 1. Environment Variable Required

**Always set `XTANK_DIR`** before running the game:
```bash
export XTANK_DIR=/path/to/xtank
./Src/xtank
```

Or run inline:
```bash
XTANK_DIR=$(pwd) ./Src/xtank
```

### 2. Header File Dependencies

**Order matters!** Some headers depend on types from others:
```c
/* Correct order */
#include "xtank.h"      /* First - defines basic types */
#include "vehicle.h"    /* Uses types from xtank.h */
#include "bullet.h"     /* Uses types from vehicle.h */
```

### 3. Pointer Aliasing in Location Updates

The `loc`/`old_loc` swapping pattern can cause bugs if you forget to update through the new pointer:
```c
/* WRONG */
temp = v->loc;
v->loc = v->old_loc;
v->old_loc = temp;
v->old_loc->x = new_x;  /* BUG: updating old location! */

/* CORRECT */
temp = v->loc;
v->loc = v->old_loc;
v->old_loc = temp;
v->loc->x = new_x;      /* Update current location */
```

### 4. Static Array Sizes

Many arrays have fixed sizes (`MAX_VEHICLES`, `MAX_BULLETS`, etc.). Exceeding these causes silent failures or corruption:
```c
/* Always check bounds */
if (num_bullets < MAX_BULLETS) {
    bullets[num_bullets++] = new_bullet;
} else {
    /* Handle overflow */
}
```

### 5. Thread Safety

Robot programs run in separate threads. Be careful with:
- Global variable access
- Shared data structures
- Race conditions in vehicle state

**Note:** Most game state is updated only in the main thread. Robot threads read state and make API calls, but don't directly modify shared structures.

### 6. X11 Color Allocation

Colors must be allocated before use. Failure to allocate causes rendering issues:
```c
/* Check return value */
if (!alloc_color(term, color_name, &color)) {
    /* Fallback to default color */
}
```

### 7. LEX-Generated Files

`setups.c` and `sload.c` are generated from `.l` files. Don't edit them directly:
```bash
# Modify setups.l, then regenerate
make clean
make
```

### 8. Platform-Specific Code

Code has platform-specific sections. When adding features, consider:
- FreeBSD vs Linux vs macOS differences
- X11 library locations (`/opt/X11` on macOS)
- Thread implementation differences
- Endianness (though game is not network-protocol strict)

### 9. Floating Point Angles

Angles use `FLOAT` type (double) and are in radians:
```c
/* Convert degrees to radians */
angle_rad = angle_deg * PI / 180.0;

/* Normalize angle to [0, 2*PI) */
while (angle < 0) angle += 2*PI;
while (angle >= 2*PI) angle -= 2*PI;
```

### 10. Memory Management

Game uses manual memory management (`malloc`/`free`). Common patterns:
```c
/* Allocation */
ptr = (Type *) malloc(sizeof(Type));
if (ptr == NULL) {
    /* Handle error */
}

/* Deallocation */
free(ptr);
ptr = NULL;  /* Prevent use-after-free */
```

### 11. Compiler Warnings Policy

**Recent development emphasizes zero warnings.** When making changes:
- Fix any new warnings introduced
- Don't ignore warnings - they often indicate real bugs
- Recent commits show pattern of warning reduction

### 12. No Standard Library Usage in Some Areas

Some game code avoids standard library to minimize dependencies:
```c
/* Custom string copy instead of strcpy */
char *my_strcpy(char *dst, const char *src) { ... }
```

### 13. Maze Coordinate System

- Origin (0,0) is **top-left**
- X increases **rightward**
- Y increases **downward**
- Angles: 0 = East, PI/2 = South, PI = West, 3PI/2 = North

### 14. Game Frame Rate

Game runs at configurable frame rate (typically 20-30 FPS):
- Weapons have cooldowns in frames
- Movement is frame-based
- Robot programs execute once per frame

---

## Quick Reference - Common Tasks

### Find a Symbol Definition
```bash
cd Src
grep -r "symbol_name" Include/
# Or use cscope
make cscope
cscope -d
```

### Add a New Source File
1. Create `Src/newsource.c`
2. Add to `Src/Makefile.inc1` in `SRCS` variable
3. Run `make clean && make`

### Change Compiler Flags
Edit `Src/Makefile.inc1`:
```make
CFLAGS += -DNEW_FLAG
```

### Debug a Segfault
```bash
gdb ./Src/xtank
(gdb) set environment XTANK_DIR $(pwd)
(gdb) run
# After crash:
(gdb) bt        # Backtrace
(gdb) info locals
(gdb) print variable_name
```

### Check Code Formatting
```bash
cd Src
indent -v file.c
diff file.c~ file.c  # See what changed
```

---

## Getting Help

### Resources
- **Mailing List**: xtank@xtank.org (subscribe at xtank-admin@xtank.org)
- **Documentation**: `Doc/` directory
- **Source Comments**: Many files have extensive inline documentation
- **Historical Notes**: `aaron_notes`, `aaron_changes`, `mark.bullen_readme`

### When Stuck
1. Read relevant documentation in `Doc/`
2. Search for similar code patterns in the codebase
3. Check `Todo/bugs` and `Todo/possible_bugs`
4. Review recent commit messages for context
5. Ask on mailing list with specific details

---

## Contributing Guidelines

### Before Submitting Changes

1. **Code Quality:**
   - Follows coding conventions
   - No new compiler warnings
   - Properly formatted (`indent`)
   - Includes copyright header

2. **Testing:**
   - Builds successfully on target platforms
   - Game runs without crashes
   - Feature works as intended
   - No memory leaks

3. **Documentation:**
   - Update relevant docs if needed
   - Clear commit message
   - Comments for complex logic

4. **Compatibility:**
   - Works on FreeBSD, Linux, macOS
   - Doesn't break existing features
   - Maintains thread safety

---

## Recent Development Patterns

Based on recent commits (as of last update):

1. **Warning Reduction** - Active effort to reduce compiler warnings
   - Recent: Display/objects.c, newconsole.c, Include/bullet.h

2. **Enum Cleanup** - Improving type safety
   - Recent: Removed NO_DIR from WallSide enum

3. **Code Formatting** - Consistent style enforcement
   - Recent: Programs/Flipper.c formatting fixes

4. **Maintenance Focus** - Stability over new features
   - Emphasis on code quality and cross-platform compatibility

---

## Version Information

**Last Updated:** 2025-11-18
**Repository State:** Clean working tree on branch `claude/claude-md-mi4jfcb7uc7e420n-01X7Vv2chucLkgs83Cfbu786`
**Recent Commits:**
- `14e6d42` - Reduce compiler warnings from Display/objects.c
- `a54536d` - Remove enum NO_DIR from WallSide, related changes
- `5244b20` - Reduce warnings in Src/newconsole.c
- `d84d513` - Fixup formatting in Programs/Flipper.c
- `375349d` - Fix one warning from Include/bullet.h declaration

---

## AI Assistant Notes

When working on this codebase as an AI assistant:

1. **Always check existing patterns** before implementing new features
2. **Respect the mature, stable nature** of the project - avoid unnecessary refactoring
3. **Test thoroughly** - this is a game with complex interactions
4. **Be mindful of threading** - robots run concurrently
5. **Consider cross-platform compatibility** - code must work on multiple OSes
6. **Follow the zero-warnings policy** - fix warnings you introduce
7. **Read historical documentation** - lots of context in aaron_notes, mark.bullen_readme
8. **Keep performance in mind** - game must run at 20-30 FPS with multiple vehicles
9. **Don't break existing saves/configs** - maintain backward compatibility where possible
10. **When in doubt, ask** - better to clarify than to introduce bugs

---

## Appendix: Key Constants

```c
/* From various headers */
#define MAX_VEHICLES      30
#define MAX_WEAPONS       6
#define MAX_TURRETS       3
#define MAX_BULLETS       200
#define GRID_WIDTH        26
#define GRID_HEIGHT       26
#define BOX_WIDTH         64
#define BOX_HEIGHT        64
#define MAX_TEAMS         6
#define MAX_TERMINALS     10
#define MAX_MESSAGES      10

/* Directions */
#define NORTH             0
#define EAST              1
#define SOUTH             2
#define WEST              3

/* Game constants */
#define FRAME_RATE        20        /* Hz */
#define MAX_GAME_SPEED    5.0       /* Boxes per second */
```

---

**End of CLAUDE.md**
