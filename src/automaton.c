/* File: automaton.c */

/*
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
 *
 * This software may be copied and distributed for educational, research,
 * and not for profit purposes provided that this copyright and statement
 * are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"

/*
 * This file includes code for automated play of Sil.
 *
 * The idea and some of the code from five of the low-level utility functions came from
 * the Angband Borg by Ben Harrison (with modifications by Dr Andrew White).
 *
 * The borrowed functions are: automaton_keypress, automaton_keypresses, automaton_inkey, automaton_flush, automaton_inkey_hack
 *
 * Inspiration to actually start writing an AI for Sil came from Brian Walker's article:
 * 'The Incredible Power of Djikstra Maps'.
 *
 * The Angband Borg was written with the idea that it would interact with the game as if it were a player
 * at a terminal. Thus, it would have to scan the screen to get information about what was going on and
 * then input characters to control the game, making it the full AI experience with no assistance from the game.
 * When it got assistance on certain occasions, this was seen as a crutch and a 'cheat' of sorts.
 *
 * I don't intend to be that masochistic. While it would be nice, the basic parts of the AI that are needed
 * (a *lot* of screen-scraping), are not very interesting. I'm more concerned with the AI needed to play Sil well
 * as opposed to working out what the letters on the screen mean. I'm thus prepared to let the AI have access to
 * internal variables when any sensible player would know their content, and even to modify the main program a bit
 * to add flags which make it easier for the AI to know what is going on. 
 *
 * However, I shall try to follow a rule that it shouldn't have access
 * to info that the player doesn't have (such as what items do before identified) and it shouldn't be able to perform
 * actions that the player can't. At some points it might break these
 * rules, but I'll flag that in the code as 'cheating' and try to remove it later.
 *
 *
 * The basic approach is that when someone activates the automaton, it replaces the standard function that gathers
 * user keypresses. Thus, whenever the program awaits input, the automaton can supply it. The automaton works out
 * what keypresses to send, and queues one or more of them up in the 'automaton_key_queue'. These are then
 * consumed by the standard game, causing it to run around etc. Real user keypresses abort the automaton and
 * return control to the user.
 *
 * There is a small amount of automaton-related code in other files, but the aim is to have almost all of it here.
 *
 *
 * phylyc (01 2017): Substantial improvement of tactical behavior
 *                   ... still under development.
 */

/*
 * {Melee, Archery, Evasion, Stealth, Perception, Will, Smithing, Song}
 */
int skill_vals[S_MAX] = {100, 50, 100, 0, 0, 50, 0, 0}; // combat values 1
// int skill_vals[S_MAX] = {100, 0, 100, 0, 0, 0, 0, 0}; // combat values 2
// int skill_vals[S_MAX] = {30, 0, 30, 100, 50, 50, 0, 0}; // stealth values

int ability_vals[S_MAX][ABILITIES_MAX] =
    {
        // Melee
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        // Archery
        {0, 0, 0, 0, 0, 0, 0, 0},
        // Evasion
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        // Stealth
        {0, 0, 0, 0, 0, 0, 0},
        // Perception
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        // Will
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        // Smithing
        {0, 0, 0, 0, 0, 0, 0, 0},
        // Song
        {0, 0, 0, 0, 0, 0, 0, 0}
    };


int light_val = 150;

#define KEY_SIZE 8192


/*
 * An array storing the automaton's additional internal info about the dungeon layout
 *
 * Currently this is just used as a boolean array for storing whether the contents of the
 * square are known (necessary because unlit floor squares can't get set with CAVE_MARK).
 *
 * It could be expanded so that each square has multiple pieces of information.
 * This could be done with bitflags (like cave_info), or even better might be an map_square
 * struct that can store numbers, indices for monsters etc. If it is too hard to get
 * a 2D array of structs to work (I couldn't do it...) then multiple 2D arrays might be best.
 */
byte (*automaton_map)[MAX_DUNGEON_WID];

int MEMORY = 2;
// player_type automaton_memory_player[2];   /* storing player info of previous turns */
// monster_type automaton_memory_monster[2][MON_MAX]; /* storing monster info of previous turns */

byte (automaton_memory_chp)[2]; /* storing chp */
// byte (automaton_memory_posy)[2][250]; /* storing y positions, 0 is player */
// byte (automaton_memory_posx)[2][250]; /* storing x positions, 0 is player */


/*
 * A Queue of keypresses to be sent
 */
static char *automaton_key_queue;
static s16b automaton_key_head;
static s16b automaton_key_tail;

/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * Add a keypress to the "queue" (fake event)
 */
errr automaton_keypress(char k)
{
    /* Hack -- Refuse to enqueue "nul" */
    if (!k) return (-1);
    
    /* Store the char, advance the queue */
    automaton_key_queue[automaton_key_head++] = k;
    
    /* Circular queue, handle wrap */
    if (automaton_key_head == KEY_SIZE) automaton_key_head = 0;
    
    /* Hack -- Catch overflow (forget oldest) */
    if (automaton_key_head == automaton_key_tail) return (-1);
    
    /* Hack -- Overflow may induce circular queue */
    if (automaton_key_tail == KEY_SIZE) automaton_key_tail = 0;
    
    /* Success */
    return (0);
}


/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * Add a keypress to the "queue" (fake event)
 */
errr automaton_keypresses(char *str)
{
    char *s;
    
    /* Enqueue them */
    for (s = str; *s; s++) automaton_keypress(*s);
    
    /* Success */
    return (0);
}


/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * Get the next automaton keypress
 */
char automaton_inkey(bool take)
{
    int i;
    
    /* Nothing ready */
    if (automaton_key_head == automaton_key_tail) return (0);
    
    /* Extract the keypress */
    i = automaton_key_queue[automaton_key_tail];
    
    /* Do not advance */
    if (!take) return (i);
    
    /* Advance the queue */
    automaton_key_tail++;
    
    /* Circular queue requires wrap-around */
    if (automaton_key_tail == KEY_SIZE) automaton_key_tail = 0;
    
    /* Return the key */
    return (i);
}


/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * Get the next automaton keypress
 */
void automaton_flush(void)
{
    /* Simply forget old keys */
    automaton_key_tail = automaton_key_head;
}


/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * Mega-Hack -- special "inkey_hack" hook.  XXX XXX XXX
 *
 * A special function hook (see "util.c") which allows the automaton to take
 * control of the "inkey()" function, and substitute in fake keypresses.
 */
extern char (*inkey_hack)(int flush_first);


/*
 * Stop the automaton.
 */
void stop_automaton(void)
{
    // set the flag to show the automaton is off
    p_ptr->automaton = FALSE;

    /* Remove hook */
    inkey_hack = NULL;
    
    /* Flush keys */
    automaton_flush();
    
    // free the "keypress queue"
    FREE(automaton_key_queue);
}


/*
 * Grid distance between two points.
 *
 * Algorithm: dist(dy,dx) = max(abs(y1 - y2), abs(x1 - x2))
 */
int grid_distance(int y1, int x1, int y2, int x2)
{
    return (((abs(y1 - y2) + abs(x1 - x2))
             + abs(abs(y1 - y2) - abs(x1 - x2))) / 2);
}


/*
 * Updates an array the size of the map with information about how long the automaton
 * thinks it will take the player to get to the given centre square from any map square.
 * 
 * The code is heavily based on update_flow() from cave.c, so see there for full comments.
 *
 * This is separated in an attempt to keep as much automaton stuff as possible out of the
 * main game files (and in the hope that the automaton flow code can be tailored in future).
 */
void update_automaton_flow(int which_flow, int cy, int cx)
{
    /*
     * int which_flow_a = FLOW_AUTOMATON;
     * int which_flow_f = FLOW_AUTOMATON_FIGHT;
     * int which_flow_s = FLOW_AUTOMATON_SECURE;
     */
    
    // paranoia
    if (!((which_flow == FLOW_AUTOMATON) ||
          (which_flow == FLOW_AUTOMATON_FIGHT) ||
          (which_flow == FLOW_AUTOMATON_SECURE)))
    {
        msg_debug("Tried to use update_automaton_flow() not with FLOW_AUTOMATON_XXX.");
        return;
    }
    
    int cost;
    
    int i, d, d2;
    byte y, x, y2, x2, y3, x3;
    int last_index;
    int grid_count = 0;
    
    /* Note where we get information from, and where we overwrite */
    int this_cycle = 0;
    int next_cycle = 1;
    
    byte flow_table[2][2][8 * FLOW_MAX_DIST];
    
    /* Save the new flow epicenter */
    flow_center_y[which_flow] = cy;
    flow_center_x[which_flow] = cx;
    update_center_y[which_flow] = cy;
    update_center_x[which_flow] = cx;
    
    /* Erase all of the current flow (noise) information */
    for (y = 0; y < p_ptr->cur_map_hgt; y++)
    {
        for (x = 0; x < p_ptr->cur_map_wid; x++)
        {
            cave_cost[which_flow][y][x] = FLOW_MAX_DIST;
        }
    }
    
    /*** Update or rebuild the flow ***/
    
    /* Store base cost at the character location */
    cave_cost[which_flow][cy][cx] = 0;
    
    /* Store this grid in the flow table, note that we've done so */
    flow_table[this_cycle][0][0] = cy;
    flow_table[this_cycle][1][0] = cx;
    grid_count = 1;
    
    /* Extend the noise burst out to its limits */
    for (cost = 1; cost <= FLOW_MAX_DIST; cost++)
    {
        /* Get the number of grids we'll be looking at */
        last_index = grid_count;
        
        /* Stop if we've run out of work to do */
        if (last_index == 0) break;
        
        /* Clear the grid count */
        grid_count = 0;
        
        /* Get each valid entry in the flow table in turn. */
        for (i = 0; i < last_index; i++)
        {
            /* Get this grid */
            y = flow_table[this_cycle][0][i];
            x = flow_table[this_cycle][1][i];
            
            // Some grids are not ready to process immediately.
            // For example doors, which add 5 cost to noise, 3 cost to movement.
            // They keep getting put back on the queue until ready.
            if (cave_cost[which_flow][y][x] >= cost)
            {
                /* Store this grid in the flow table */
                flow_table[next_cycle][0][grid_count] = y;
                flow_table[next_cycle][1][grid_count] = x;
                
                /* Increment number of grids stored */
                grid_count++;
            }
            // if the grid is ready to process...
            else
            {
                /* Look at all adjacent grids */
                for (d = 0; d < 8; d++)
                {
                    int extra_cost = 0;
                    
                    bool next_to_wall = FALSE;
                    
                    /* Child location */
                    y2 = y + ddy_ddd[d];
                    x2 = x + ddx_ddd[d];
                    
                    /* Check Bounds */
                    if (!in_bounds(y2, x2)) continue;
                    
                    /* Ignore previously marked grids, unless this is a shorter distance */
                    if (cave_cost[which_flow][y2][x2] < FLOW_MAX_DIST) continue;
                    
                    // skip unknown grids
                    if (!((cave_info[y2][x2] & (CAVE_MARK)) || automaton_map[y2][x2])) continue;
                    
                    // skip walls
                    if (cave_wall_bold(y2, x2)) continue;
                    
                    // skip chasms
                    if (cave_feat[y2][x2] == FEAT_CHASM) continue;
                    
                    // skip rubble
                    if (cave_feat[y2][x2] == FEAT_RUBBLE) continue;
                    
                    // penalise traps
                    if (cave_trap_bold(y2, x2) && !(cave_info[y2][x2] & (CAVE_HIDDEN)))
                    {
                        extra_cost += 3;
                    }
                    
                    if (cave_m_idx[y2][x2] > 0)
                    {
                        monster_type *n_ptr = &mon_list[cave_m_idx[y2][x2]];
                        monster_race *q_ptr = &r_info[n_ptr->r_idx];
                        
                        // penalise visible unmoving monsters
                        // except right besides us
                        if ((q_ptr->flags1 & RF1_NEVER_MOVE) && (cost > 1))
                        {
                            // this brings the cost to lock as target over 12
                            extra_cost += 10;
                        }
                        
                        // penalise visible unaware monsters
                        if (n_ptr->alertness < ALERTNESS_ALERT)
                        {
                            extra_cost += 3;
                        }
                        
                        // secure: avoid monsters
                        if (which_flow == FLOW_AUTOMATON_SECURE) extra_cost += 25;
                    }
                    
                    // penalise squares next to monsters
                    // penalise squares not next to walls
                    for (d2 = 0; d2 < 8; d2++)
                    {
                        /* Grand-child location */
                        y3 = y2 + ddy_ddd[d2];
                        x3 = x2 + ddx_ddd[d2];
                        
                        if (cave_m_idx[y3][x3] > 0)
                        {
                            monster_type *n_ptr = &mon_list[cave_m_idx[y3][x3]];
                            monster_race *q_ptr = &r_info[n_ptr->r_idx];
                            
                            // penalise squares next to visible unmoving monsters
                            // except right besides us
                            if ((q_ptr->flags1 & RF1_NEVER_MOVE) && (cost > 1))
                            {
                                extra_cost += 1;
                            }
                            
                            // penalise squares next to visible melee monsters
                            if ((n_ptr->ml) && (q_ptr->freq_ranged == 0))
                            {
                                extra_cost += 2;
                            }
                            
                            // penalise squares for each visible unalert monsters next to it
                            if ((n_ptr->ml) && (n_ptr->alertness < ALERTNESS_ALERT))
                            {
                                extra_cost += 1;
                            }
                            
                            // secure: avoid monsters
                            if (which_flow == FLOW_AUTOMATON_SECURE) extra_cost += 2;
                        }
                        
                        if (cave_wall_bold(y3, x3))
                        {
                            next_to_wall = TRUE;
                        }
                    }
                    
                    // penalise squares not next to walls
                    // but only if there is no monster on it where we are standing right beside it
                    if ((!next_to_wall) && (cave_m_idx[y2][x2] == 0) && (cost > 1))
                    {
                        extra_cost += 1;
                    }
                    
                    /* Store cost at this location */
                    cave_cost[which_flow][y2][x2] = cost + extra_cost;
                    
                    /* Store this grid in the flow table */
                    flow_table[next_cycle][0][grid_count] = y2;
                    flow_table[next_cycle][1][grid_count] = x2;
                    
                    /* Increment number of grids stored */
                    grid_count++;
                }
            }
        }
        
        if ((which_flow == FLOW_AUTOMATON_SECURE) && (p_ptr->chp < automaton_memory_chp[0]))
        {
            cave_cost[which_flow][p_ptr->py][p_ptr->px] += 1;
        }
        
        /* Swap write and read portions of the table */
        if (this_cycle == 0)
        {
            this_cycle = 1;
            next_cycle = 0;
        }
        else
        {
            this_cycle = 0;
            next_cycle = 1;
        }
    }
}


/*
 * The automaton keeps an internal map to remind it of various things.
 *
 * This function gets it to remember squares that were seen at some point.
 */
void add_seen_squares_to_map(void)
{
    int y, x;
    
    // look at every unmarked square of the map
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            if (cave_info[y][x] & (CAVE_SEEN))
            {
                automaton_map[y][x] = TRUE;
            }
        }
    }
    
    // add own square to map too (it doesn't count as SEEN)
    automaton_map[p_ptr->py][p_ptr->px] = TRUE;
}


void find_secure_position(int **ty, int **tx)
{
    int i, d2;
//  int d;
    byte y, x;
    byte y3, x3;
    
    int dist;
    int best_dist = FLOW_MAX_DIST - 1;
//  int com_dist;
//  int best_com_dist = 0;
    
    int center_of_monsters_x = 0;
    int center_of_monsters_y = 0;
    int monster_count = 0;
    
    int com_dist;
    int extra_cost;
    int adj_wall_count;
    int best_wall_count = 0;
    
    monster_type *m_ptr;
    
    /* determining center of monsters */
    for (i = 1; i < mon_max; i++)
    {
        m_ptr = &mon_list[i];
        
        // Skip dead monsters
        if (!m_ptr->r_idx) continue;
        
        // Skip unseen monsters
        // todo: add memory of seen monsters
        if (!m_ptr->ml) continue;
        
        // todo: trace arrows
        
        center_of_monsters_x += m_ptr->fx;
        center_of_monsters_y += m_ptr->fy;
        
        monster_count++;
    }
    
    if (monster_count > 0)
    {
        center_of_monsters_x /= monster_count;
        center_of_monsters_y /= monster_count;
    }
    
    // msg_debug("com: %d, %d", center_of_monsters_y, center_of_monsters_x);
    
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            //  Check Bounds
            if (!in_bounds(y, x)) continue;
            
            // skip unknown grids
            if (!((cave_info[y][x] & (CAVE_MARK)) || automaton_map[y][x])) continue;
            
            // skip walls
            if (cave_wall_bold(y, x)) continue;
            
            // skip chasms
            if (cave_feat[y][x] == FEAT_CHASM) continue;
            
            // skip rubble
            if (cave_feat[y][x] == FEAT_RUBBLE) continue;
            
            // skip monsters
            // if (cave_m_idx[y][x] > 0) continue;
            
            // distance from center_of_monsters
            com_dist = 0;
            extra_cost = 0;
            
            // secure: penalise proximity to center_of_monsters on the grid
            if (monster_count > 0)
            {
                // com_dist = distance(center_of_monsters_y, center_of_monsters_x, y, x);
                com_dist = grid_distance(center_of_monsters_y, center_of_monsters_x, y, x);
                if (com_dist < 20)
                {
                    // using += generates crash
                    extra_cost = extra_cost + (20 - com_dist);
                }
            }
            
            adj_wall_count = 0;
            
            // counting of adjecent walls
            for (d2 = 0; d2 < 8; d2++)
            {
                /* Grand-child location */
                y3 = y + ddy_ddd[d2];
                x3 = x + ddx_ddd[d2];
                
                // prefer to have as many surrounding walls as possible
                if (cave_wall_bold(y3, x3)) adj_wall_count += 1;
            }
            
            dist = flow_dist(FLOW_AUTOMATON_SECURE, y, x);
            
            dist += extra_cost;
            
            // preferring nearer positions with same wall count
            // preferring positions further away if they have more walls
            if (((adj_wall_count == best_wall_count) && (dist <  best_dist)) ||
                ((adj_wall_count > MIN(best_wall_count, 6)) &&
                 (dist < MIN((best_dist + 1) * 10, FLOW_MAX_DIST - 1))
                 ))
            {
                best_wall_count = adj_wall_count;
                best_dist = dist;
                **ty = y;
                **tx = x;
                
                // msg_debug("%d", best_dist);
            }
        }
    }
    
    // msg_debug("%d", best_dist);
    
    if (ty != 0) target_set_location(**ty, **tx);
}


// best fighting position is next to many walls
void find_fighting_position(int **ty, int **tx)
{
    int y, x, d2;
    byte y3, x3;
    
    int adj_wall_count;
    int best_wall_count = 0;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1;
    
    int adj_monster_count = 0;
    
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            /* Check Bounds */
            if (!in_bounds(y, x)) continue;
            
            // can't see unmarked things...
            if (!(cave_info[y][x] & (CAVE_MARK))) continue;
            
            // skip walls
            if (cave_wall_bold(y, x)) continue;
            
            // skip chasms
            if (cave_feat[y][x] == FEAT_CHASM) continue;
            
            // skip rubble
            if (cave_feat[y][x] == FEAT_RUBBLE) continue;
            
            adj_wall_count = 0;
            
            // counting of adjecent walls
            for (d2 = 0; d2 < 8; d2++)
            {
                /* Grand-child location */
                y3 = y + ddy_ddd[d2];
                x3 = x + ddx_ddd[d2];
                
                // prefer to have as many surrounding walls as possible
                if (cave_wall_bold(y3, x3)) adj_wall_count += 1;
            }
            
            dist = flow_dist(FLOW_AUTOMATON_FIGHT, y, x);
            
            // preferring nearer positions with same wall count
            // preferring positions further away if they have more walls
            //  (6 is good enough)
            if (((adj_wall_count == best_wall_count) && (dist < best_dist)) ||
                ((adj_wall_count > MIN(best_wall_count, 6)) &&
                 (dist < MIN((best_dist + 1) * 10, FLOW_MAX_DIST - 1)) ))
            {
                best_wall_count = adj_wall_count;
                best_dist = dist;
                **ty = y;
                **tx = x;
            }
        }
    }
    
    // msg_debug("%d", best_dist);
    
    // count adjecent monsters to player
    for (d2 = 0; d2 < 8; d2++)
    {
        /* Grand-child location */
        y3 = p_ptr->py + ddy_ddd[d2];
        x3 = p_ptr->px + ddx_ddd[d2];
        
        if (cave_m_idx[y3][x3] > 0) adj_monster_count += 1;
    }
    
    // if there is only one adjecent monster, actual best position is the current position
    // this will lead in fighting_strategy to attack this monster
    if (adj_monster_count <= 1)
    {
        **ty = p_ptr->py;
        **tx = p_ptr->px;
    }
    
    if (ty != 0) target_set_location(**ty, **tx);
}


bool find_enemy_to_kill(int **ty, int **tx)
{
    int i;
    int dist;
    int best_dist = 12; // target to beat
                        // shortbows have 12 range
    int grid_dist;
    
    bool can_fire = FALSE;
    bool only_smart = TRUE;

    monster_type *m_ptr;
    monster_race *r_ptr;
    
    // if we are afraid we cannot fight
    if (p_ptr->afraid) return (FALSE);
    
    for (i = 1; i < mon_max; i++)
    {
        m_ptr = &mon_list[i];
        r_ptr = &r_info[m_ptr->r_idx];
        
        // Skip dead monsters
        if (!m_ptr->r_idx) continue;
        
        // Skip unseen and unalert monsters: Don't skip alert Archers hiding in the dark!
        // Cheat - player may not have knowledge about their true positions
        if (!m_ptr->ml && (m_ptr->alertness < ALERTNESS_ALERT)) continue;
        
        // Skip unseen melee monsters
        if (!m_ptr->ml && (r_ptr->freq_ranged == 0)) continue;
        
        // Skip unalert monsters
        if (m_ptr->alertness < ALERTNESS_ALERT) continue;
        
        dist = flow_dist(FLOW_AUTOMATON_FIGHT, m_ptr->fy, m_ptr->fx);
        
        if (!(r_ptr->flags2 & (RF2_SMART))) only_smart = FALSE;
        
        if (dist < best_dist)
        {
            // better: selection by dangerousness 
            best_dist = dist;
            
            // penalise monsters in the dark
            if (!m_ptr->ml) best_dist += 2;
            
            // distance to walk on the grid: max(|py - fy|, |px - fx|)
            grid_dist = grid_distance(p_ptr->py, p_ptr->px, m_ptr->fy, m_ptr->fx);
            
            // don't fire at monsters you don't see
            // (can be improved! sometimes it is good to shoot into the dark)
            can_fire = ((grid_dist > 1) && (m_ptr->ml) && (!p_ptr->blind) && (cave_info[m_ptr->fy][m_ptr->fx] & (CAVE_FIRE)));
            
            // wait for melee & alert & moving & not fleeing & not smart opponents to come to you
            if ((grid_dist < 4) && (grid_dist > 1) && (r_ptr->freq_ranged == 0) &&
                (m_ptr->alertness >= ALERTNESS_ALERT) && !(r_ptr->flags1 & RF1_NEVER_MOVE) &&
                (m_ptr->stance != STANCE_FLEEING) && !only_smart)
            {
                **ty = p_ptr->py;
                **tx = p_ptr->px;
            }
            /*
            // still wait if below 50% health
            else if (p_ptr->chp * 100 / p_ptr->mhp < 50)
            {
                **ty = p_ptr->py;
                **tx = p_ptr->px;
            }
             */
            // charge at the others
            else
            {
                **ty = m_ptr->fy;
                **tx = m_ptr->fx;
            }
        }
    }
    
    // if (best_dist < 12) msg_debug("%d", best_dist);
    
    if (can_fire && inventory[INVEN_BOW].tval &&
        ((inventory[INVEN_QUIVER1].number >= 1) || (inventory[INVEN_QUIVER2].number >= 1)))
    {
        // clear the target
        // todo: select target
        target_set_monster(0);
        
        // queue the commands
        if (inventory[INVEN_QUIVER1].number >= 1)
        {
            automaton_keypresses("ff");
        }
        else // inventory[INVEN_QUIVER2].number >= 1
        {
            automaton_keypresses("Ff");
        }
        
        return (TRUE);
    }
    
    return (FALSE);
}


bool fighting_strategy(int *ty, int *tx)
{
    int i;
    int monster_threat = 0;
    int monster_count = 0;
    
    int dist;
    int best_dist = 20; // only monsters within flow distance of 20
                        // are accounted for calculating the threat
    
    int grid_dist;
    bool only_ranged = TRUE;
    bool chased = FALSE;
    
    bool end_turn = FALSE;

    monster_type *m_ptr;
    monster_race *r_ptr;
    
    object_type *o_ptr = &inventory[INVEN_LITE];
    
    /* don't fight without light 
     * or if starving
     */
    if ((((o_ptr->sval == SV_LIGHT_TORCH) || (o_ptr->sval == SV_LIGHT_LANTERN)) &&
         (o_ptr->timeout == 0)) ||
        (p_ptr->food < 1))
    {
        return (FALSE);
    }
    
    // stay where you are if you are confused!
    if (p_ptr->confused)
    {
        if (ty != 0) target_set_location(*ty, *tx);
        
        // proceede with random movement if lost health
        if (p_ptr->chp < automaton_memory_chp[0])
        {
            end_turn = find_enemy_to_kill(&ty, &tx);
            
            return (end_turn);
        }
        
        return (FALSE);
    }
    
    // we better search for a weapon first ...
    if ((&inventory[INVEN_WIELD])->weight == 0) return (FALSE);
    
    // determining the threat of nearby monsters
    for (i = 1; i < mon_max; i++)
    {
        m_ptr = &mon_list[i];
        r_ptr = &r_info[m_ptr->r_idx];
        
        // Skip dead monsters
        if (!m_ptr->r_idx) continue;
        
        // Skip unseen monsters
        if (!m_ptr->ml) continue;
        
        // Skip unalert monsters
        if (m_ptr->alertness < ALERTNESS_ALERT) continue;
        
        // Skip unmoving monsters except if directly beside them
        if (r_ptr->flags1 & RF1_NEVER_MOVE)
        {
            // distance to walk on the grid: max(|py - fy|, |px - fx|)
            grid_dist = grid_distance(p_ptr->py, p_ptr->px, m_ptr->fy, m_ptr->fx);
            
            if (grid_dist > 1) continue;
        }
        
        dist = flow_dist(FLOW_AUTOMATON_FIGHT, m_ptr->fy, m_ptr->fx);
        
        if (dist < best_dist)
        {
            // could use mon_power instead of level, player has no insight in that though
            monster_threat += r_ptr->level;
            monster_count += 1;
        }
        
        if (r_ptr->freq_ranged == 0) only_ranged = FALSE;
        
        // being chased by faster monsters
        if (r_ptr->speed > p_ptr->pspeed)
        {
            // distance to walk on the grid: max(|py - fy|, |px - fx|)
            grid_dist = grid_distance(p_ptr->py, p_ptr->px, m_ptr->fy, m_ptr->fx);
            
            if (grid_dist == 1) chased = TRUE;
        }
    }
    
    // run away if afraid or below 30% health but not chased by faster monsters
    if ((p_ptr->afraid) ||
        ((p_ptr->chp * 100 / p_ptr->mhp < 30) && !chased && !only_ranged))
    {
        // msg_debug("secure");
        find_secure_position(&ty, &tx);
        
        // proceede with killing monsters if already at best position and not afraid
        if ((*ty == p_ptr->py) && (*tx == p_ptr->px) && (!p_ptr->afraid))
        {
            // msg_debug("fight s");
            end_turn = find_enemy_to_kill(&ty, &tx);
            
            return (end_turn);
        }

        return (FALSE);
    }
    
    // if monster threat is greater than your level (depth)
    // or if below 75% health (likely trigger for smart monsters to engage)
    // then find a good fighting position
    // always engage if only archers are around though
    if (((monster_count > 1) && (monster_threat > p_ptr->depth * 10 / 10) && (!only_ranged)) ||
        (p_ptr->chp * 100 / p_ptr->mhp < 75))
    {
        // msg_debug("pos");
        find_fighting_position(&ty, &tx);
        
        // proceede with killing monsters if already at best position
        if ((*ty == p_ptr->py) && (*tx == p_ptr->px))
        {
            // msg_debug("fight p");
            end_turn = find_enemy_to_kill(&ty, &tx);
            
            return (end_turn);
        }
        else
        {
            return (FALSE);
        }
    }
    else
    {
        // msg_debug("fight");
        end_turn = find_enemy_to_kill(&ty, &tx);
        
        return (end_turn);
    }
}


/*
 * Taken from object2.c and modified values
 * Originally, value was adjusted subject to boni to base values
 * but base cost from template is rather arbitrary, so we adapt
 * our own evaluation algorithm.
 */
/*
 * Return the "value" of an "unknown" item
 * Make a guess at the value of non-aware items
 */
static int object_value_base_auto(const object_type *o_ptr)
{
    int value = 1;
    u32b f1, f2, f3;
    
    // object_kind *k_ptr = &k_info[o_ptr->k_idx];
    
    // extract the flags for the object
    // need to be careful using these as they could involve hidden information
    object_flags(o_ptr, &f1, &f2, &f3);
    
    // 'nothings' are worthless
    if (!o_ptr->k_idx) return (0);
    
    int ds = 0;
    
    /* for damage */
    if (wield_slot(o_ptr) == INVEN_BOW)
    {
        ds = total_ads(o_ptr, FALSE);
    }
    else
    {
        ds = strength_modified_ds(o_ptr, 0);
    }
    int max_dam = o_ptr->dd * ds;
    int min_dam = (o_ptr->ds > 0) ? o_ptr->dd : 0;
    
    /* for protection */
    int max_prt = o_ptr->pd * o_ptr->ps;
    int min_prt = (o_ptr->ps > 0) ? o_ptr->pd : 0;
    
    /* value for aware objects & all others */
    if (object_aware_p(o_ptr) || TRUE)
    {
        /* Give credit for hit bonus */
        value += (o_ptr->att * 100);
        
        /* Give credit for max damage */
        value += (max_dam * 110 / 2);
        
        /* Give credit for min damage */
        value += (min_dam * 100 / 2);
        
        /* Give credit for evasion bonus */
        value += (o_ptr->evn * 100);
        
        /* Give credit for max protection */
        value += (max_prt *  90 / 2);
        
        /* Give credit for min protection */
        value += (min_prt * 100 / 2);
        
        if (wield_slot(o_ptr) == INVEN_WIELD || wield_slot(o_ptr) == INVEN_BOW)
        {
            // value weight being same as strength
            value *= 100 / (70 + abs( p_ptr->stat_use[A_STR] - o_ptr->weight / 10 ) * 21 / 10 );
            
            // value one-handedness
            if (f3 & (TR3_TWO_HANDED)) value *= 8 / 10;
        }
        
        switch (o_ptr->tval)
        {
                /* base value for various things */
            case TV_ARROW: value += 100;
            case TV_FOOD: value += 5;
            case TV_POTION: value += 20;
            case TV_STAFF: value += 70;
            case TV_HORN: value += 90;
            case TV_RING: value += 45;
            case TV_AMULET: value += 45;
                /* light */
            case TV_LIGHT:
            {
                switch (o_ptr->sval)
                {
                        /* light_val = 150 */
                    case SV_LIGHT_TORCH:
                        value = RADIUS_TORCH * light_val;
                        value -= (FUEL_TORCH - o_ptr->timeout) * light_val / FUEL_TORCH;
                        break;
                    case SV_LIGHT_LANTERN:
                        value = RADIUS_LANTERN * light_val;
                        value -= (FUEL_LAMP - o_ptr->timeout) * light_val / FUEL_LAMP;
                        break;
                    case SV_LIGHT_LESSER_JEWEL:
                        value = RADIUS_LESSER_JEWEL * light_val;
                        break;
                    case SV_LIGHT_FEANORIAN:
                        value = RADIUS_FEANORIAN * light_val;
                        break;
                    case SV_LIGHT_SILMARIL:
                        value = RADIUS_SILMARIL * light_val;
                        break;
                }
            }
        }
        
        // value unknown {special} items as 100
        if ((o_ptr->name1 || o_ptr->name2) && !object_known_p(o_ptr))
        {
            value += 100;
        }
    }
    
    return (value);
}


/*
 * Taken from object2.c and modified values
 */
static int object_value_real_auto(const object_type *o_ptr)
{
    int value;
    
    u32b f1, f2, f3;
    
    object_kind *k_ptr = &k_info[o_ptr->k_idx];
    
    /* Hack -- "worthless" items */
    if (!k_ptr->cost) return (0);
    
    /* base value */
    value = 1;
    
    /* Extract some flags */
    object_flags(o_ptr, &f1, &f2, &f3);
    
    /* Analyze pval bonus */
    switch (o_ptr->tval)
    {
        case TV_ARROW:
        case TV_BOW:
        case TV_DIGGING:
        case TV_HAFTED:
        case TV_POLEARM:
        case TV_SWORD:
        case TV_BOOTS:
        case TV_GLOVES:
        case TV_HELM:
        case TV_CROWN:
        case TV_SHIELD:
        case TV_CLOAK:
        case TV_SOFT_ARMOR:
        case TV_MAIL:
        case TV_LIGHT:
        case TV_AMULET:
        case TV_RING:
        {
            /* Hack -- Negative "pval" is always bad */
            if (o_ptr->pval < 0) return (0);
            
            /* No pval */
            if (!o_ptr->pval) break;
            
            /* Give credit for TR1 Flags */
            if (f1 & (TR1_STR)) value += (o_ptr->pval * 300);
            if (f1 & (TR1_DEX)) value += (o_ptr->pval * 300);
            if (f1 & (TR1_CON)) value += (o_ptr->pval * 300);
            if (f1 & (TR1_GRA)) value += (o_ptr->pval * 300);
            if (f1 & (TR1_NEG_STR)) value -= (o_ptr->pval * 300);
            if (f1 & (TR1_NEG_DEX)) value -= (o_ptr->pval * 300);
            if (f1 & (TR1_NEG_CON)) value -= (o_ptr->pval * 300);
            if (f1 & (TR1_NEG_GRA)) value -= (o_ptr->pval * 300);
            if (f1 & (TR1_MEL)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_ARC)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_STL)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_PER)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_WIL)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_SMT)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_SNG)) value += (o_ptr->pval * 100);
            if (f1 & (TR1_TUNNEL)) value += (o_ptr->pval * 50);
            if (f1 & (TR1_SHARPNESS)) value += (200);
            if (f1 & (TR1_SHARPNESS2)) value += (400);
            if (f1 & (TR1_VAMPIRIC)) value += (300);
            if (f1 & (TR1_SLAY_ORC)) value += (100);
            if (f1 & (TR1_SLAY_TROLL)) value += (100);
            if (f1 & (TR1_SLAY_WOLF)) value += (100);
            if (f1 & (TR1_SLAY_SPIDER)) value += (100);
            if (f1 & (TR1_SLAY_UNDEAD)) value += (100);
            if (f1 & (TR1_SLAY_RAUKO)) value += (200);
            if (f1 & (TR1_SLAY_DRAGON)) value += (200);
            if (f1 & (TR1_BRAND_COLD)) value += (250);
            if (f1 & (TR1_BRAND_FIRE)) value += (250);
            if (f1 & (TR1_BRAND_ELEC)) value += (250);
            if (f1 & (TR1_BRAND_POIS)) value += (250);
            if (f1 & (TR1_ALL_STATS)) value += (o_ptr->pval * 1200);
            
            /* Give credit for TR2 Flags */
            if (f2 & (TR2_SUST_STR)) value += 100;
            if (f2 & (TR2_SUST_DEX)) value += 100;
            if (f2 & (TR2_SUST_CON)) value += 150;
            if (f2 & (TR2_SUST_GRA)) value += 100;
            if (f2 & (TR2_RES_COLD)) value += 250;
            if (f2 & (TR2_RES_FIRE)) value += 250;
            if (f2 & (TR2_RES_ELEC)) value += 250;
            if (f2 & (TR2_RES_POIS)) value += 250;
            if (f2 & (TR2_RES_DARK)) value += 250;
            if (f2 & (TR2_RES_FEAR)) value += 250;
            if (f2 & (TR2_RES_BLIND)) value += 200;
            if (f2 & (TR2_RES_CONFU)) value += 200;
            if (f2 & (TR2_RES_STUN)) value += 200;
            if (f2 & (TR2_RES_HALLU)) value += 200;
            if (f2 & (TR2_RADIANCE)) value += 100;
            if (f2 & (TR2_SLOW_DIGEST)) value += 150;
            if (f2 & (TR2_LIGHT)) value += 300;
            if (f2 & (TR2_REGEN)) value += 400;
            if (f2 & (TR2_SEE_INVIS)) value += 350;
            if (f2 & (TR2_FREE_ACT)) value += 200;
            if (f2 & (TR2_SPEED)) value += 1000;
            if (f2 & (TR2_FEAR)) value -= 500;
            if (f2 & (TR2_HUNGER)) value -= 300;
            if (f2 & (TR2_DARKNESS))
            {
                value -= 400;
                if (p_ptr->cur_light <= 1) value -= 10000;
            }
            if (f2 & (TR2_SLOWNESS)) value -= 500;
            if (f2 & (TR2_DANGER)) value -= 300;
            if (f2 & (TR2_AGGRAVATE)) value -= 500;
            if (f2 & (TR2_HAUNTED)) value -= 500;
            if (f2 & (TR2_VUL_COLD)) value -= 250;
            if (f2 & (TR2_VUL_FIRE)) value -= 250;
            if (f2 & (TR2_VUL_POIS)) value -= 250;
            if (f2 & (TR2_SUST_STATS)) value = 450;
            if (f2 & (TR2_RESISTANCE)) value = 750;
            
            /* Give credit for (some) TR3 Flags */
            if (f2 & (TR3_MITHRIL)) value += 100;
            if (f2 & (TR3_THROWING)) value += 100;
            if (f2 & (TR3_LIGHT_CURSE)) value -= 300;
            if (f2 & (TR3_HEAVY_CURSE)) value -= 600;
            if (f2 & (TR3_PERMA_CURSE)) value -= 1000;
            
            break;
        }
    }
    
    
    /* Analyze the item */
    switch (o_ptr->tval)
    {
            /* Staffs */
        case TV_STAFF:
        {
            /* Pay extra for charges, depending on standard number of
             * charges.  Handle new-style wands correctly.
             */
            value += ((value / 20) * (o_ptr->pval / o_ptr->number));
            
            /* Done */
            break;
        }
            
            /* Rings/Amulets */
        case TV_RING:
        case TV_AMULET:
        {
            /* Hack -- negative bonuses are bad */
            if (o_ptr->att < 0) return (0);
            if (o_ptr->evn < 0) return (0);
            
            /* Compute base bonus */
            value += object_value_base_auto(o_ptr);
            
            break;
        }
            
            /* Everything else */
        case TV_ARROW:
        case TV_BOW:
        case TV_DIGGING:
        case TV_HAFTED:
        case TV_POLEARM:
        case TV_SWORD:
        case TV_BOOTS:
        case TV_GLOVES:
        case TV_HELM:
        case TV_CROWN:
        case TV_SHIELD:
        case TV_CLOAK:
        case TV_SOFT_ARMOR:
        case TV_MAIL:
        case TV_LIGHT:
        default:
        {
            /* Compute base bonus */
            value += object_value_base_auto(o_ptr);
            
            /* Done */
            break;
        }
    }
    
    /* No negative value */
    if (value < 0) value = 0;
    
    /* Return the value */
    return (value);
}


/*
 * Return the price of an item including plusses (and charges).
 *
 * This function returns the "value" of the given item (qty one).
 *
 * Never notice "unknown" bonuses or properties, including "curses",
 * since that would give the player information he did not have.
 *
 * Note that discounted items stay discounted forever.
 */
int object_value_auto(const object_type *o_ptr)
{
    int value;
    
    /* Known items -- acquire the actual value */
    if (object_known_p(o_ptr))
    {
        /* Broken items -- worthless */
        if (broken_p(o_ptr)) return (0);
        
        /* Cursed items -- worthless */
        // if (cursed_p(o_ptr)) return (0);
        
        /* Real value (see above) */
        value = object_value_real_auto(o_ptr);
    }
    /* Unknown items -- acquire the base value */
    else
    {
        /* Hack -- Felt broken items */
        if ((o_ptr->ident & (IDENT_SENSE)) && broken_p(o_ptr)) return (0);
        
        /* Hack -- Felt cursed items */
        if ((o_ptr->ident & (IDENT_SENSE)) && cursed_p(o_ptr)) return (0);
        
        /* Base value (see above) */
        value = object_value_base_auto(o_ptr);
    }
    
    /* Return the final value */
    return (value);
}



int evaluate_object(object_type *o_ptr)
{
    int slot = wield_slot(o_ptr);
    int value = 0;
    object_type *o_ptr2;

    if (slot >= 0)
    {
        o_ptr2 = &inventory[slot];
        
        switch (slot)
        {
            case INVEN_WIELD:
            case INVEN_BOW:
            case INVEN_LEFT:
            case INVEN_RIGHT:
            case INVEN_NECK:
            case INVEN_LITE:
            case INVEN_BODY:
            case INVEN_OUTER:
            case INVEN_ARM:
            case INVEN_HEAD:
            case INVEN_HANDS:
            case INVEN_FEET:
                value = object_value_auto(o_ptr) - object_value_auto(o_ptr2);
                break;
            case INVEN_QUIVER1:
            case INVEN_QUIVER2:
                // hack: the +1 means we will accept ties (to restock arrows)
                // in the future it is better to do arrows differently
                // by counting those in pack and considering how much we care about archery
                value = object_value_auto(o_ptr) - object_value_auto(o_ptr2) + 1;
                break;
        }
    }
    // no food, potions etc. atm
    else
    {
        value = 0;
    }
    
    return (value);
}


void find_object(int *ty, int *tx)
{
    int i;
    int y, x;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1; // target to beat
    int value;
    
	/* Scan objects */
	for (i = 1; i < o_max; i++)
	{
		object_type *o_ptr = &o_list[i];
        
		/* Skip dead objects */
		if (!o_ptr->k_idx) continue;
        
		/* Skip held objects */
		if (o_ptr->held_m_idx) continue;
        
		/* Location */
		y = o_ptr->iy;
		x = o_ptr->ix;
        
		// skip items whose location is unknown
		if (!o_ptr->marked) continue;

        // skip items in the player's square
		if ((y == p_ptr->py) && (x == p_ptr->px)) continue;

        dist = flow_dist(FLOW_AUTOMATON, y, x);
        
        value = evaluate_object(o_ptr) - (dist / 20);
        
        // msg_debug("%d", value);
        
        // don't seek boring items
        if (value <= 0) continue;
        
        if (dist < best_dist)
        {
            best_dist = dist;
            
            *ty = y;
            *tx = x;
        }
	}
    
    if (ty != 0) target_set_location(*ty, *tx);

}


bool pickup_object(void)
{
    object_type *o_ptr = &o_list[cave_o_idx[p_ptr->py][p_ptr->px]];
    char commands[80];
    int value = evaluate_object(o_ptr);
    
    if (value > 0)
    {
        int slot = wield_slot(o_ptr);
     
        // if it is a non-wieldable item
        if (slot == -1)
        {
            // create the commands
            strnfmt(commands, sizeof(commands), "g-");
            
            // queue the commands
            automaton_keypresses(commands);
        }
        
        // special rules for arrows
        else if ((slot == INVEN_QUIVER1) || (slot == INVEN_QUIVER2))
        {
            // if it is considered equal in value to existing arrows, then just get it to allow auto-merging
            if (value == 1)
            {
                // create the commands
                strnfmt(commands, sizeof(commands), "g-");
                
                // queue the commands
                automaton_keypresses(commands);
            }
            
            // otherwise it is considered better than the existing arrows so wield it...
            else
            {
                // create the commands
                strnfmt(commands, sizeof(commands), "w-");
                
                // queue the commands
                automaton_keypresses(commands);
                
                // if both slots are full, we need to tell the game to replace the inferior one
                if (inventory[INVEN_QUIVER1].k_idx && inventory[INVEN_QUIVER2].k_idx)
                {
                    if (evaluate_object(&inventory[INVEN_QUIVER1]) > evaluate_object(&inventory[INVEN_QUIVER2]))
                    {
                        automaton_keypress(index_to_label(INVEN_QUIVER2));
                    }
                    else
                    {
                        automaton_keypress(index_to_label(INVEN_QUIVER1));
                    }
                }
            }
        }
        
        // default for wieldable items
        else
        {
            // create the commands
            strnfmt(commands, sizeof(commands), "w-");
            
            // queue the commands
            automaton_keypresses(commands);
        }
        

        return (TRUE);
    }
    
    return (FALSE);
}


bool renew_light(void)
{
    int i;
    bool found = FALSE;
    object_type *o_ptr = &inventory[INVEN_LITE];
    char commands[80];
    
    // if we need a new light source
    if (((o_ptr->sval == SV_LIGHT_TORCH) || (o_ptr->sval == SV_LIGHT_LANTERN)) && (o_ptr->timeout < 110))
    {
        // special case when using a lantern
        if (o_ptr->sval == SV_LIGHT_LANTERN)
        {
            // find a lantern or flask in the pack
            for (i = 0; i < INVEN_PACK; i++)
            {
                o_ptr = &inventory[i];
                
                if ((o_ptr->tval == TV_FLASK) ||
                   ((o_ptr->tval == TV_LIGHT) && (o_ptr->sval == SV_LIGHT_LANTERN) && (o_ptr->timeout > 0)))
                {
                    found = TRUE;
                    break;
                }
            }
        }
        
        // if we haven't yet found anything
        if (!found)
        {
            // find one or more light sources in the pack
            for (i = 0; i < INVEN_PACK; i++)
            {
                o_ptr = &inventory[i];
                
                if (o_ptr->tval == TV_LIGHT)
                {
                    if (((o_ptr->sval != SV_LIGHT_TORCH) &&
                         (o_ptr->sval != SV_LIGHT_LANTERN)) || (o_ptr->timeout > 100))
                    {
                        found = TRUE;
                        break;
                    }
                }
            }
        }
        
        // desperation!
        if (!found && (inventory[INVEN_LITE].timeout == 0))
        {
            // find one or more light sources in the pack
            for (i = 0; i < INVEN_PACK; i++)
            {
                o_ptr = &inventory[i];
                
                if (o_ptr->tval == TV_LIGHT)
                {
                    if (o_ptr->timeout > 0)
                    {
                        found = TRUE;
                        break;
                    }
                }
            }
        }
        
        // use the new light if found
        if (found)
        {
            // create the commands
            strnfmt(commands, sizeof(commands), "u%c", 'a' + i);
            
            // queue the commands
            automaton_keypresses(commands);
            
            return (TRUE);
        }
    }
    
    return (FALSE);
}


void rest(int *ty, int *tx)
{
    // if we are not poisoned but still lost health during this turn
    if ((!p_ptr->poisoned) && (automaton_memory_chp[0] > p_ptr->chp))
    {
        return;
    }
    
    /* only rest if no health is lost from previous turn to now */
    if ((p_ptr->chp * 100 / p_ptr->mhp < 75) ||
        (p_ptr->stun) || (p_ptr->confused) || (p_ptr->afraid) ||
        (p_ptr->blind) || (p_ptr->image) || (p_ptr->slow) || (p_ptr->cut) ||
        (p_ptr->poisoned))
    {
        *ty = p_ptr->py;
        *tx = p_ptr->px;
    }
}


bool eat_food(void)
{
    int i;
    bool found = FALSE;
    object_type *o_ptr;
    char commands[80];
    
    // if hungry
    if (p_ptr->food < 2000)
    {
        // find some food in the pack
        for (i = 0; i < INVEN_PACK; i++)
        {
            o_ptr = &inventory[i];
            
            if (o_ptr->tval == TV_FOOD)
            {
                found = TRUE;
                break;
            }
        }
        
        // use food if found
        if (found)
        {
            // create the commands
            strnfmt(commands, sizeof(commands), "u%c", 'a' + i);
            
            // queue the commands
            automaton_keypresses(commands);
            
            return (TRUE);
        }
    }
    
    return (FALSE);
}


void find_unexplored(int *ty, int *tx)
{
    int y, x, yy, xx;
    int i;
    int dist;
    // don't walk 50 grids to explore one more room
    int best_dist = 50;
    int count = 0;
    
    // first, if you are in a suspicious dead-end corridor, possibly search a bit
    
    // count adjacent walls
    for (i = 7; i >= 0; i--)
    {
        // get the adjacent location
        y = p_ptr->py + ddy_ddd[i];
        x = p_ptr->px + ddx_ddd[i];
        
        if (cave_wall_bold(y,x) && (cave_feat[y][x] != FEAT_RUBBLE)) count++;
    }
    
    // if it looks like there must be a secret door ...
    // ... and you are actually able to detect it ...
    if ((count == 7) && (p_ptr->skill_use[S_PER] > 5 + p_ptr->depth / 2))
    {
        *ty = p_ptr->py;
        *tx = p_ptr->px;
    }
    
    // if you are just in an everyday location...
    else
    {
        // look at every unmarked square of the map
        for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
        {
            for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
            {
                if (!((cave_info[y][x] & (CAVE_MARK)) || automaton_map[y][x]))
                {
                    int best_local_dist = FLOW_MAX_DIST - 1;
                    
                    // ignore your own square
                    if ((y == p_ptr->py) && (x == p_ptr->px)) continue;
                    
                    // try all adjacent locations
                    for (i = 7; i >= 0; i--)
                    {
                        // get the adjacent location
                        yy = y + ddy_ddd[i];
                        xx = x + ddx_ddd[i];
                        
                        dist = flow_dist(FLOW_AUTOMATON, yy, xx);
                        
                        // keep track of the best square adjacent to the unmarked location that is reachable from the player
                        if (dist < best_local_dist) best_local_dist = dist;
                    }
                    
                    // keep track of the closest unmarked location
                    // (the second line breaks ties in a way that makes it go around corridor corners properly)
                    if ((best_local_dist < best_dist) ||
                        ((best_local_dist == best_dist) && (best_local_dist == 1) && (count != 7) &&
                         ((y == p_ptr->py) || (x == p_ptr->px)) && (cave_info[y][x] & (CAVE_VIEW))))
                    {
                        best_dist = best_local_dist;
                        *ty = y;
                        *tx = x;
                    }
                }
            }
        }
    }
    
    if (ty != 0) target_set_location(*ty, *tx);
}


void find_secret_door(int *ty, int *tx)
{
    // don't waste turns if perception is too low to be able to detect secret doors
    if (p_ptr->skill_use[S_PER] < 5 + p_ptr->depth / 2)
    {
        return;
    }
    
    int y, x, yy, xx;
    int i;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1;
    
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            if (cave_floorlike_bold(y,x))
            {
                int count = 0;
                
                // count adjacent walls
                for (i = 7; i >= 0; i--)
                {
                    // get the adjacent location
                    yy = y + ddy_ddd[i];
                    xx = x + ddx_ddd[i];
                    
                    if (cave_wall_bold(yy,xx) && (cave_feat[yy][xx] != FEAT_RUBBLE)) count++;
                }
                
                // if it looks like there must be a secret door ...
                // ... and you are actually able to detect it ...
                // ... without spending ages searching for it ...
                if (count == 7)
                {
                    dist = flow_dist(FLOW_AUTOMATON, y, x);
                    
                    // keep track of the closest likely secret door location
                    if (dist < best_dist)
                    {
                        best_dist = dist;
                        *ty = y;
                        *tx = x;
                    }
                }
            }
        }
    }
    
    if (ty != 0) target_set_location(*ty, *tx);
}


bool leave_level(void)
{
    if ((cave_feat[p_ptr->py][p_ptr->px] == FEAT_MORE) || (cave_feat[p_ptr->py][p_ptr->px] == FEAT_MORE_SHAFT) ||
        (cave_feat[p_ptr->py][p_ptr->px] == FEAT_LESS) || (cave_feat[p_ptr->py][p_ptr->px] == FEAT_LESS_SHAFT))
    {
        automaton_keypresses(",y");
        return (TRUE);
    }
    
    return (FALSE);
}


void find_stairs_down(int *ty, int *tx)
{
    int y, x;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1;
    
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            // can't see unmarked things...
            if (!(cave_info[y][x] & (CAVE_MARK))) continue;
            
            if ((cave_feat[y][x] == FEAT_MORE) || (cave_feat[y][x] == FEAT_MORE_SHAFT))
            {
                dist = flow_dist(FLOW_AUTOMATON, y, x);
                
                if (dist < best_dist)
                {
                    best_dist = dist;
                    *ty = y;
                    *tx = x;
                }
            }
        }
    }
    
    if (ty != 0) target_set_location(*ty, *tx);
}


void find_stairs_up(int *ty, int *tx)
{
    int y, x;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1;
    
    for (y = 1; y < p_ptr->cur_map_hgt - 1; y++)
    {
        for (x = 1; x < p_ptr->cur_map_wid - 1; x++)
        {
            // can't see unmarked things...
            if (!(cave_info[y][x] & (CAVE_MARK))) continue;
            
            if ((cave_feat[y][x] == FEAT_LESS) || (cave_feat[y][x] == FEAT_LESS_SHAFT))
            {
                dist = flow_dist(FLOW_AUTOMATON, y, x);
                
                if (dist < best_dist)
                {
                    best_dist = dist;
                    *ty = y;
                    *tx = x;
                }
            }
        }
    }
    
    if (ty != 0) target_set_location(*ty, *tx);
}


bool allocate_experience(void)
{
//    return (FALSE);
    
    char commands[80];
    int i;
    int val;
    int best_val = 0;
    int best_skill = S_MEL;
    
    // find the most important skill to raise
    for (i = 0; i < S_MAX; i++)
    {
        val = skill_vals[i] * 10 / (p_ptr->skill_base[i] + 1);
        
        if (val > best_val)
        {
            best_val = val;
            best_skill = i;
        }
    }
    
    if (p_ptr->new_exp >= (p_ptr->skill_base[best_skill] + 1) * 100)
    {
        // create the first command (getting to the increase skills screen)
        strnfmt(commands, sizeof(commands), "@i");
        
        // queue it
        automaton_keypresses(commands);

        // move to the appropriate skill
        for (i = 0; i < best_skill; i++)
        {
            automaton_keypress('2');
        }

        // create the second command (increasing the skill and exiting)
        strnfmt(commands, sizeof(commands), "6\r\033");  // \033 is ESCAPE
        
        // queue it
        automaton_keypresses(commands);

        return (TRUE);
    }
    
    return (FALSE);
}


/*
 * AI to-do list:
 *
 * Memory
 *  - remember monsters
 *
 * Exploring
 *  - be less completist with levels (don't walk 50 squares to explore one more room...)
 *  + be willing to use up stairs (or chasms/shafts) sometimes
 *  + stop searching for secret doors after a while if nothing found
 *      (only search if perception is high enough)
 *  - don't run into pairs of non-moving monsters & retreat loop
 *
 * Stealth
 *  - stay away from nearby unalert monsters
 *  + stay near walls
 *
 * Combat
 *  + learn to rest properly (away from archers in the dark)
 *  - fight from more sensible locations (use hallways)
 *  - engage smart monsters if they are waiting outside of corridor
 *      (implement memory)
 *  - choose targets for archery
 *  - deal with 'afraid' status
 *      (use proper flow for next step)
 *  - wield new arrows from pack when a quiver is empty
 *  + don't endlessly run away from monsters that are faster
 *  - use potions against negative effects
 *  - deal better with 'blind' status
 *
 * Experience
 *  - gain abilities
 *
 * Darkness
 *  + ignores monsters in darkness (e.g. archers & shadow molds)
 *      [needs to be improved without cheating!]
 *  + often dies while ignoring shadow molds and archers!
 *
 * Objects
 *  + can't deal with rings and amulets properly
 *  - can't deal with non-wieldable items
 *  - remove items if bad
 *  - never drops items
 *  + renew light or eat even in combat situations if its critical
 *
 * Loops
 *  - pillared rooms and archers which run can cause it to walk back and forth
 *  + stuck searching for a secret door to get to the down stairs
 *  - stuck in middle of symmetric orc configuration, can't decide which one to approach
 *
 *
 * In general, I wrote most of the basic routine as an AI with no internal state.
 * I've added state in the form of its internal map and more state should be added
 * (for example so it can work out if it lost health since last turn, or to set its
 *  own internal mode).
 */


/*
 * Take an AI controlled turn.
 */
void automaton_turn(void)
{
    int y, x;
    int i;
    // int j;
    int ty = 0;
    int tx = 0;
    int dist;
    int best_dist = FLOW_MAX_DIST - 1; // default to an easy-to-beat value
    int best_dir = 5;                   // default to not moving
    bool found_direction = FALSE;
    
    char base_command = ';';
    char commands[80];
    
    add_seen_squares_to_map();

    // generate flow maps from the player
    update_automaton_flow(FLOW_AUTOMATON, p_ptr->py, p_ptr->px);
    update_automaton_flow(FLOW_AUTOMATON_FIGHT, p_ptr->py, p_ptr->px);
    update_automaton_flow(FLOW_AUTOMATON_SECURE, p_ptr->py, p_ptr->px);
    
    // allocate experience
    if (ty == 0)    if (allocate_experience())  return;
    
    // otherwise: fight monsters or get to a better position
    if (ty == 0)    if (fighting_strategy(&ty, &tx))  return;
    
    // otherwise: get item if standing on a good one
    if (ty == 0)    if (pickup_object())  return;
    
    // otherwise: light a torch if needed
    if (ty == 0)    if (renew_light())  return;
    
    // otherwise: renew arrows if needed
    // if no arrows are in quiver, then only search for arrows in inventory
    //      if we picked up arrows and so know that we actually have some
    // if (ty == 0)    if (renew_arrows())  return;

    // otherwise: rest if less than 75% health or with negative timed effects
    if (ty == 0)    rest(&ty, &tx);
    
    // otherwise: eat something from inventory if hungry
    if (ty == 0)    if (eat_food())  return;
    
    // otherwise: find an object worth taking
    if (ty == 0)    find_object(&ty, &tx);
    
    // otherwise: find the closest unexplored location that is next to an explored one
    if (ty == 0)    find_unexplored(&ty, &tx);
    
    // otherwise: take stairs if standing on them
    if (ty == 0)    if (leave_level())  return;
    
    // otherwise: head for the down stairs if not too far ahead yet
    if ((ty == 0) && (p_ptr->depth < 3 + min_depth()))    find_stairs_down(&ty, &tx);
    
    // otherwise: find a plausible location for a secret door
    if (ty == 0)    find_secret_door(&ty, &tx);
    
    // otherwise: take stairs if standing on them
    if (ty == 0)    if (leave_level())  return;
    
    // otherwise: head for the down stairs
    if (ty == 0)    find_stairs_up(&ty, &tx);
    
    // exit automaton mode if no known target is found
    if (ty == 0)
    {
        msg_print("Could not find anything to do.");
        stop_automaton();
        return;
    }
    
    // find direction to the target: easy if you are already there!
    if ((ty == p_ptr->py) && (tx == p_ptr->px))
    {
        found_direction = TRUE;
    }
    // find direction to target
    /*
     * the path to the target is computed with different flows
     * depending on the situation!
     */
    else
    {
        // generate flow maps towards this target
        update_automaton_flow(FLOW_AUTOMATON, ty, tx);
        update_automaton_flow(FLOW_AUTOMATON_FIGHT, ty, tx);
        update_automaton_flow(FLOW_AUTOMATON_SECURE, ty, tx);
        
        // work out the adjacent square closest to the target (with preference for orthogonals)
        for (i = 7; i >= 0; i--)
        {
            // get the location
            y = p_ptr->py + ddy_ddd[i];
            x = p_ptr->px + ddx_ddd[i];
            
            // make sure it is in bounds
            if (!in_bounds(y, x)) continue;
            
            // determine how far it is from the target
            dist = flow_dist(FLOW_AUTOMATON, y, x);
            
            // if it is at least as good as anything so far, remember it
            if (dist <= best_dist)
            {
                found_direction = TRUE;
                best_dist = dist;
                best_dir = ddd[i];
            }
        }
    }
    
    // exit if no known target is found
    if (!found_direction)
    {
        msg_print("Could not work out which way to proceed.");
        stop_automaton();
        return;
    }

    // choose this best direction
    y = p_ptr->py + ddy[best_dir];
    x = p_ptr->px + ddx[best_dir];
    
    // sometimes bash doors
    if (cave_known_closed_door_bold(y, x))
    {
        if (one_in_(5)) base_command = '/';
    }
    
    // create the commands
    strnfmt(commands, sizeof(commands), "%c%c", base_command, '0' + best_dir);
    
    // queue the commands
    automaton_keypresses(commands);

    // if searching, we know that adjacent unmarked squares must be passable
    if ((base_command == ';') && (best_dir == 5))
    {
        // work out the adjacent square closest to the target (with preference for orthogonals)
        for (i = 7; i >= 0; i--)
        {
            // get the location
            y = p_ptr->py + ddy_ddd[i];
            x = p_ptr->px + ddx_ddd[i];
            
            automaton_map[y][x] = TRUE;
        }
    }
    
    // say 'yes' to visible traps
    if (cave_trap_bold(y,x) && !(cave_info[y][x] & (CAVE_HIDDEN)))
    {
        automaton_keypress('y');
    }
    
    /* 
     * updating the memory
     */
    
    for (i = MEMORY; i > 0; i--)
    {
        automaton_memory_chp[i] = automaton_memory_chp[i-1];
        
        /*
        for (j = 0; j < MEMORY_POS; j++)
        {
            automaton_memory_posy[i][j] = automaton_memory_posy[i-1][j];
            automaton_memory_posx[i][j] = automaton_memory_posx[i-1][j];
        }
         */
    }
    
    // position 0 is for player
    automaton_memory_chp[0] = p_ptr->chp;
    
    /*
    automaton_memory_posy[0][0] = p_ptr->py;
    automaton_memory_posx[0][0] = p_ptr->px;
    
    j = 1;
    monster_type *m_ptr;
    
    // hoping that there are less than MEMORY_POS monsters that we actually see
    for (i = 1; i < mon_max; i++)
    {
        m_ptr = &mon_list[i];
        
        // Skip dead monsters
        if (!m_ptr->r_idx) continue;
        
        // Skip unseen monsters
        if (!m_ptr->ml) continue;
        
        automaton_memory_posy[0][j] = m_ptr->fy;
        automaton_memory_posx[0][j] = m_ptr->fx;
        
        j++;
    }
    // fill the rest with 0
    for (i = j+1; i < MEMORY_POS; i++)
    {
        automaton_memory_posy[0][i] = 0;
        automaton_memory_posx[0][i] = 0;
    }
     */
    
}




/*
 * (From the Angband Borg by Ben Harrison & Dr Andrew White)
 *
 * This function lets the automaton "steal" control from the user.
 *
 * The "z-term.c" file provides a special hook which we use to
 * bypass the standard "Term_flush()" and "Term_inkey()" functions
 * and replace them with the function below.
 *
 * The only way that the automaton can be stopped once it is started,
 * unless it dies or encounters an error, is to press any key.
 * This function checks for user input on a regular basic, and
 * when any is encountered, it relinquishes control gracefully.
 *
 * Note that this function hook automatically removes itself when
 * it realizes that it should no longer be active.  Note that this
 * may take place after the game has asked for the next keypress,
 * but the various "keypress" routines should be able to handle this.
 *
 * XXX XXX XXX We do not correctly handle the "take" flag
 */
char automaton_inkey_hack(int flush_first)
{
	int i;
    char ch;
    
	// paranoia
	if (!p_ptr->automaton)
	{
        stop_automaton();
        
		/* Nothing ready */
		return (ESCAPE);
	}
    
    // flush key buffer if requested
    if (flush_first)
    {
        // only flush if needed
        if (automaton_inkey(FALSE) != 0)
        {
            // Flush keys
            ////automaton_flush();   currently not actually doing this as it stops us queuing a 'y' for stepping on traps
        }
    }
    
	// check for manual user abort
	(void)Term_inkey(&ch, FALSE, TRUE);
    
    // if a key is hit, stop the automaton
    if (ch > 0)
    {
        stop_automaton();
        return (ESCAPE);
    }
    
	// check for a previously queued key, without taking it from the queue
	i = automaton_inkey(FALSE);
    
	// if it was empty and we need more keys
	if (!i)
	{
        // handle waiting for commands separately
        if (waiting_for_command)
        {
            // takes its turn by choosing some keys representing commands and queuing them
            automaton_turn();
            
            // pause for a moment so the user can see what is happening
            Term_xtra(TERM_XTRA_DELAY, OPT_delay_factor_auto * op_ptr->delay_factor);
        }
        
        else
        {
            /* Hack -- Process events (do not wait) */
            (void)Term_xtra(TERM_XTRA_EVENT, FALSE);
            
            stop_automaton();
            return (ESCAPE);
        }
	}
    
	// check for a previously queued key, taking it from the queue
	i = automaton_inkey(TRUE);
    
	// deal with empty queue
	if (!i)
	{
		// exit
		return('\0');
	}
    
	// return the key chosen
	return (i);
}



/*
 * Turn the automaton on.
 */
void do_cmd_automaton(void)
{
    int y, x;
    
    // set the flag to show the automaton is on
    p_ptr->automaton = TRUE;

    // empty the "keypress queue"
    automaton_flush();
    
    // allocate the "keypress queue"
    C_MAKE(automaton_key_queue, KEY_SIZE, char);

    // allocate automaton map
    C_MAKE(automaton_map, MAX_DUNGEON_HGT, byte_wid);
    
    // initialize automaton map
    for (y = 0; y < MAX_DUNGEON_HGT; y++)
        for (x = 0; x < MAX_DUNGEON_WID; x++)
        {
            automaton_map[y][x] = FALSE;
        }
    
    // activate the key stealer
    inkey_hack = automaton_inkey_hack;
}



