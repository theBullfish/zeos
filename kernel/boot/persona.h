/*
 * Zeos — Shell Persona System
 *
 * Three views of the same shell. Same OS. Same capabilities.
 * The persona is a filter on what's surfaced, not what exists.
 *
 * Zeros:  robotics/hardware-first. scan, motors, sensors, build, flash.
 * DereZ:  code/AI-first. run, debug, trace, signal, view-source.
 * Full:   everything. The curtain is up. You see all of it.
 *
 * "raise" lifts the curtain from any persona to full.
 * "zeros" or "derez" drops into that persona.
 * Students choose. The OS never locks them out.
 */

#ifndef ZEOS_PERSONA_H
#define ZEOS_PERSONA_H

/* The three personas */
enum persona {
    PERSONA_ZEROS,      /* Robotics / hardware / build */
    PERSONA_DEREZ,      /* Code / AI / debug */
    PERSONA_FULL,       /* Everything — curtain raised */
};

/* Command visibility flags */
#define VIS_ZEROS    (1 << 0)   /* Visible in Zeros persona */
#define VIS_DEREZ    (1 << 1)   /* Visible in DereZ persona */
#define VIS_FULL     (1 << 2)   /* Visible only when curtain is raised */
#define VIS_ALWAYS   (VIS_ZEROS | VIS_DEREZ | VIS_FULL)

/* A shell command entry */
struct shell_cmd {
    const char     *name;       /* Command name */
    const char     *desc;       /* Short description */
    void          (*handler)(const char *args);  /* Handler (args = rest of line) */
    uint8_t         vis;        /* Visibility flags */
};

/* Shell prompt per persona */
static inline const char *persona_prompt(enum persona p)
{
    switch (p) {
    case PERSONA_ZEROS: return "zeros> ";
    case PERSONA_DEREZ: return "derez> ";
    case PERSONA_FULL:  return "zeos> ";
    }
    return "zeos> ";
}

/* Banner per persona */
static inline const char *persona_banner(enum persona p)
{
    switch (p) {
    case PERSONA_ZEROS:
        return "  [ Zeros — Robotics Mode ]\n"
               "  Hardware, sensors, motors. Type 'raise' to see everything.\n\n";
    case PERSONA_DEREZ:
        return "  [ DereZ — Dev Mode ]\n"
               "  Code, signals, debug. Type 'raise' to see everything.\n\n";
    case PERSONA_FULL:
        return "  [ Zeos — Full System ]\n"
               "  Curtain raised. Everything visible.\n\n";
    }
    return "";
}

/* Check if a command is visible in the current persona */
static inline int cmd_visible(const struct shell_cmd *cmd, enum persona p)
{
    switch (p) {
    case PERSONA_ZEROS: return (cmd->vis & VIS_ZEROS) != 0;
    case PERSONA_DEREZ: return (cmd->vis & VIS_DEREZ) != 0;
    case PERSONA_FULL:  return 1;  /* Full always sees everything */
    }
    return 0;
}

#endif /* ZEOS_PERSONA_H */
