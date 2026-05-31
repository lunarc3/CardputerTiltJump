/**
 * Tilt Jump — M5CardputerADV Edition
 *
 * Gravity-tilt controlled jumping game
 * Hardware: M5Cardputer (ESP32-S3 + MPU6886 IMU + 240x135 LCD)
 *
 * Controls: Tilt left/right to move
 *           Any key → Start / Restart
 */

#include <M5Cardputer.h>

/* =========================================================
 *  Constants — tweak these to adjust game feel
 * ========================================================= */

// Screen
static constexpr int SW = 240;
static constexpr int SH = 135;

// Platforms
static constexpr int PLAT_CNT = 14;   // On-screen platform count
static constexpr int PLAT_W   = 28;   // Platform width
static constexpr int PLAT_H   = 5;    // Platform height

// Player
static constexpr int DOOD_W = 12;
static constexpr int DOOD_H = 14;

// Physics
static constexpr float GRAVITY   = 0.24f;  // Gravity acceleration
static constexpr float JUMP_VEL  = -8.0f;  // Jump initial velocity (negative = up)
static constexpr float TILT_GAIN = 0.82f;  // Tilt sensitivity (lower = smoother acceleration)
static constexpr float FRICTION  = 0.90f;  // Horizontal friction (higher = more inertia)
static constexpr float MAX_VEL_X = 12.0f;  // Max horizontal velocity

// Particles
static constexpr int MAX_PART = 12;

/* =========================================================
 *  Type definitions
 * ========================================================= */

// Platform types: Normal / Breakable / Moving
enum PlatKind : uint8_t { PLAT_NORMAL, PLAT_BREAK, PLAT_MOVE };

struct Platform {
    float    x, y;
    float    speed;   // Horizontal speed for PLAT_MOVE
    PlatKind kind;
    bool     alive;   // Set to false when PLAT_BREAK is stepped on
};

struct Particle {
    float   x, y, vy;
    uint8_t ttl;      // Time to live in frames
};

/* =========================================================
 *  Global state
 * ========================================================= */

static M5Canvas canvas(&M5Cardputer.Display);

static Platform platforms[PLAT_CNT];
static Particle particles[MAX_PART];

static float playerX, playerY;   // World coordinates
static float velX, velY;         // Velocity
static float cameraY;            // Camera offset
static float tiltBaseline;       // Calibrated rest tilt value

static int32_t       score;
static int32_t       highScore;
static bool          isDead;
static bool          isTitleScreen;
static unsigned long deathTime;

// Zone tracker for even platform distribution
static int lastZone = 0;

/* =========================================================
 *  Platform management
 * ========================================================= */

// Get the Y coordinate of the highest platform (Y axis: down = positive, smaller = higher)
static float highestPlatformY() {
    float top = 1e9f;
    for (int i = 0; i < PLAT_CNT; i++)
        if (platforms[i].alive && platforms[i].y < top)
            top = platforms[i].y;
    return top;
}

// Randomly choose a platform type based on score (higher score = more special platforms)
static PlatKind randomKind(int sc) {
    if (sc < 500) return PLAT_NORMAL;
    int r = random(100);
    if (sc < 1500)
        return r < 70 ? PLAT_NORMAL : r < 90 ? PLAT_BREAK : PLAT_MOVE;
    return r < 50 ? PLAT_NORMAL : r < 75 ? PLAT_BREAK : PLAT_MOVE;
}

// Create a platform at the specified Y coordinate
// Uses zone-based placement to avoid clustering
static void createPlatform(int i, float y, PlatKind kind) {
    // Divide screen into 5 horizontal zones for even distribution
    const int zones = 5;
    const int zoneW = (SW - PLAT_W) / zones;

    // Pick a zone different from the last one
    int zone;
    do {
        zone = random(zones);
    } while (zone == lastZone && zones > 1);
    lastZone = zone;

    // Random position within that zone
    platforms[i].x     = zone * zoneW + random(0, zoneW);
    platforms[i].y     = y;
    platforms[i].speed = (random(2) ? 1.f : -1.f) *
                         (0.4f + random(60) / 100.f);
    platforms[i].kind  = kind;
    platforms[i].alive = true;
}

/* =========================================================
 *  Particle system (visual feedback for jumps and breaks)
 * ========================================================= */

static void spawnParticles(float x, float y, int count) {
    for (int j = 0; j < count; j++)
        for (int i = 0; i < MAX_PART; i++)
            if (particles[i].ttl == 0) {
                particles[i] = {
                    x + (float)random(-4, 4),
                    y + (float)random(-2, 2),
                    0.3f + random(80) / 200.f,
                    (uint8_t)(15 + random(12))
                };
                break;
            }
}

static void updateParticles() {
    for (int i = 0; i < MAX_PART; i++)
        if (particles[i].ttl > 0) {
            particles[i].y += particles[i].vy;
            particles[i].ttl--;
        }
}

static void renderParticles() {
    for (int i = 0; i < MAX_PART; i++) {
        if (particles[i].ttl == 0) continue;
        int sy = (int)(particles[i].y - cameraY);
        if (sy < 0 || sy >= SH) continue;
        int b = constrain((int)particles[i].ttl * 10, 0, 255);
        // RGB565 grayscale
        uint16_t col = ((b >> 3) << 11) | ((b >> 2) << 5) | (b >> 3);
        canvas.fillRect((int)particles[i].x, sy, 2, 2, col);
    }
}

/* =========================================================
 *  Tilt calibration
 *  Reads current orientation at game start as "level zero"
 *  If left/right is reversed, change ax below to -ax or ay
 * ========================================================= */

static void calibrateTilt() {
    float ax, ay, az, sum = 0;
    for (int i = 0; i < 30; i++) {
        M5.Imu.getAccel(&ax, &ay, &az);
        sum += ax;   // ← If reversed, change to ay or -ax
        delay(5);
    }
    tiltBaseline = sum / 30.f;
}

/* =========================================================
 *  Init / Reset
 * ========================================================= */

static void resetGame() {
    playerX = SW / 2.f;
    playerY = SH - 24.f;
    velX = velY = 0;
    cameraY = 0;
    score = 0;
    isDead = false;
    lastZone = 0;
    memset(particles, 0, sizeof(particles));

    // First platform directly under the player
    createPlatform(0, SH - 8.f, PLAT_NORMAL);
    platforms[0].x = playerX - PLAT_W / 2.f;

    // Remaining platforms scattered upward
    float y = platforms[0].y;
    for (int i = 1; i < PLAT_CNT; i++) {
        y -= random(20, 38);
        createPlatform(i, y, randomKind(0));
    }

    calibrateTilt();
}

/* =========================================================
 *  Physics update (called once per frame)
 * ========================================================= */

static void updatePhysics() {
    if (isDead) return;

    /* -- Tilt input → horizontal acceleration -- */
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    float tilt = ax - tiltBaseline;

    velX -= tilt * TILT_GAIN;
    velX *= FRICTION;
    if (velX >  MAX_VEL_X) velX =  MAX_VEL_X;
    if (velX < -MAX_VEL_X) velX = -MAX_VEL_X;

    /* -- Gravity & displacement -- */
    velY += GRAVITY;
    playerX += velX;
    playerY += velY;

    /* -- Wrap horizontally across the screen -- */
    if (playerX < -DOOD_W) playerX = SW;
    if (playerX >  SW)     playerX = -DOOD_W;

    /* -- Moving platform animation -- */
    for (int i = 0; i < PLAT_CNT; i++) {
        if (platforms[i].kind == PLAT_MOVE && platforms[i].alive) {
            platforms[i].x += platforms[i].speed;
            if (platforms[i].x < 0 || platforms[i].x > SW - PLAT_W)
                platforms[i].speed = -platforms[i].speed;
        }
    }

    /* -- Platform collision (only while falling) -- */
    if (velY > 0) {
        for (int i = 0; i < PLAT_CNT; i++) {
            if (!platforms[i].alive) continue;

            float top = platforms[i].y;
            if (playerX + DOOD_W / 2.f > platforms[i].x          &&
                playerX - DOOD_W / 2.f < platforms[i].x + PLAT_W &&
                playerY + DOOD_H >= top                           &&
                playerY + DOOD_H <= top + PLAT_H + velY + 1.f)
            {
                // Breakable platform: shatter and spawn particles
                if (platforms[i].kind == PLAT_BREAK) {
                    platforms[i].alive = false;
                    spawnParticles(platforms[i].x + PLAT_W / 2.f, top, 6);
                }
                // Bounce
                velY = JUMP_VEL;
                playerY = top - DOOD_H;
                spawnParticles(playerX, playerY + DOOD_H, 3);
                break;
            }
        }
    }

    /* -- Camera follow (scroll when player reaches upper 1/3 of screen) -- */
    if (playerY - cameraY < SH / 3)
        cameraY = playerY - SH / 3;

    /* -- Scoring -- */
    int heightScore = (int)(-cameraY);
    if (heightScore > score) score = heightScore;
    if (score > highScore) highScore = score;

    /* -- Recycle platforms that leave the screen to the top -- */
    for (int i = 0; i < PLAT_CNT; i++) {
        if (!platforms[i].alive || platforms[i].y - cameraY > SH + 30) {
            int gap = random(16, 30);
            if (score > 2000) gap += random(0, 4);
            if (score > 4000) gap += random(0, 4);
            if (score > 6000) gap += random(0, 4);
            if (gap > 44) gap = 44;  // Ensure gap never exceeds max jump height
            createPlatform(i, highestPlatformY() - gap, randomKind(score));
        }
    }

    /* -- Death check: fell off the bottom of the screen -- */
    if (playerY - cameraY > SH + 50) {
        isDead = true;
        deathTime = millis();
    }

    updateParticles();
}

/* =========================================================
 *  Game screen rendering
 * ========================================================= */

static void renderGame() {
    canvas.fillSprite(0x0841);  // Dark blue background

    /* -- Platforms -- */
    for (int i = 0; i < PLAT_CNT; i++) {
        if (!platforms[i].alive) continue;
        int sx = (int)platforms[i].x;
        int sy = (int)(platforms[i].y - cameraY);
        if (sy < -PLAT_H || sy > SH) continue;

        uint16_t col;
        switch (platforms[i].kind) {
            case PLAT_NORMAL: col = 0x07E0; break;  // Green
            case PLAT_BREAK:  col = 0xFD20; break;  // Orange
            case PLAT_MOVE:   col = 0x4D9F; break;  // Sky blue
        }
        canvas.fillRoundRect(sx, sy, PLAT_W, PLAT_H, 2, col);
        canvas.fillRect(sx + 2, sy, PLAT_W - 4, 1, 0xFFFF);  // Highlight line
    }

    /* -- Particles -- */
    renderParticles();

    /* -- Player (pixel-art bird) -- */
    int dx = (int)(playerX - DOOD_W / 2);
    int dy = (int)(playerY - cameraY - DOOD_H);
    int faceDir = (velX > 0.5f) ? 1 : (velX < -0.5f) ? -1 : 0;

    // Feet
    canvas.fillRect(dx + 2, dy + DOOD_H - 3, 3, 3, 0xFA00);
    canvas.fillRect(dx + DOOD_W - 5, dy + DOOD_H - 3, 3, 3, 0xFA00);
    // Body
    canvas.fillRect(dx + 2, dy + 5, DOOD_W - 4, DOOD_H - 8, 0xFFE0);
    // Head
    canvas.fillRoundRect(dx + 1, dy, DOOD_W - 2, 8, 3, 0xFFE0);
    // Eyes (offset toward movement direction)
    canvas.fillRect(dx + 3 + faceDir, dy + 2, 2, 2, 0x2000);
    canvas.fillRect(dx + DOOD_W - 5 + faceDir, dy + 2, 2, 2, 0x2000);
    // Beak (facing movement direction)
    if (faceDir > 0)
        canvas.fillRect(dx + DOOD_W, dy + 4, 3, 2, 0xFD20);
    else if (faceDir < 0)
        canvas.fillRect(dx - 3, dy + 4, 3, 2, 0xFD20);

    /* -- HUD -- */
    canvas.setCursor(4, 3);
    canvas.setTextColor(0xFFFF, 0x0841);
    canvas.setTextSize(1);
    canvas.printf("%d", score);
    canvas.setCursor(SW - 44, 3);
    canvas.printf("HI:%d", highScore);

    /* -- Game Over overlay -- */
    if (isDead) {
        canvas.fillRoundRect(35, 22, 170, 85, 10, 0x2104);
        canvas.drawRoundRect(35, 22, 170, 85, 10, 0x7BEF);

        canvas.setTextColor(0xF800);
        canvas.setTextSize(2);
        canvas.setCursor(56, 30);
        canvas.print("GAME OVER");

        canvas.setTextColor(0xFFFF);
        canvas.setTextSize(1);
        canvas.setCursor(65, 54);
        canvas.printf("Score: %d", score);
        canvas.setCursor(65, 68);
        canvas.printf("Best:  %d", highScore);

        // Blinking prompt
        if ((millis() / 400) % 2) {
            canvas.setTextColor(0x07E0);
            canvas.setCursor(52, 90);
            canvas.print("Press any key");
        }
    }

    canvas.pushSprite(0, 0);
}

/* =========================================================
 *  Title screen
 * ========================================================= */

static void renderTitleScreen() {
    canvas.fillSprite(0x0000);
    unsigned long t = millis();

    // Animated background: slowly drifting semi-transparent platforms
    for (int i = 0; i < 5; i++) {
        int bx = (SW / 5 * i + (int)(t / (30 + i * 10)) % 60) %
                  (SW + 30) - 15;
        canvas.fillRoundRect(bx, 30 + i * 20, PLAT_W, PLAT_H, 2, 0x0220);
    }

    // Title
    canvas.setTextColor(0xFFE0);
    canvas.setTextSize(2);
    canvas.setCursor(34, 8);
    canvas.print("Tilt Jump");

    // Subtitle
    canvas.setTextColor(0x07E0);
    canvas.setTextSize(1);
    canvas.setCursor(64, 30);
    canvas.print("Cardputer Adv");

    // Instructions
    canvas.setTextColor(0xBDF7);
    canvas.setCursor(28, 50);
    canvas.print("Tilt device to move");
    canvas.setCursor(50, 63);
    canvas.print("Jump on platforms!");

    // Platform legend
    canvas.fillRoundRect(20, 80, PLAT_W, PLAT_H, 2, 0x07E0);
    canvas.setCursor(52, 81);
    canvas.setTextColor(0x8410);
    canvas.print("Normal");

    canvas.fillRoundRect(100, 80, PLAT_W, PLAT_H, 2, 0xFD20);
    canvas.setCursor(132, 81);
    canvas.print("Break");

    canvas.fillRoundRect(170, 80, PLAT_W, PLAT_H, 2, 0x4D9F);
    canvas.setCursor(202, 81);
    canvas.print("Move");

    // Blinking "press any key"
    if ((t / 500) % 2) {
        canvas.setTextColor(0xFFFF);
        canvas.setCursor(52, 108);
        canvas.print("Press any key");
    }

    canvas.pushSprite(0, 0);
}

/* =========================================================
 *  Main entry point
 * ========================================================= */

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);        // Same init as SSHClient
    M5Cardputer.Display.setRotation(1);  // Landscape
    M5Cardputer.Display.setTextSize(1);

    canvas.createSprite(SW, SH);         // Double-buffered off-screen canvas

    highScore = 0;
    isTitleScreen = true;
    randomSeed(millis());
}

void loop() {
    M5Cardputer.update();  // Same as SSHClient: refresh device state each frame

    /* -- Title screen -- */
    if (isTitleScreen) {
        renderTitleScreen();
        // Same keyboard detection pattern as SSHClient
        if (M5Cardputer.Keyboard.isChange() &&
            M5Cardputer.Keyboard.isPressed()) {
            isTitleScreen = false;
            resetGame();
        }
        delay(30);
        return;
    }

    /* -- Game loop -- */
    if (!isDead) updatePhysics();
    renderGame();

    /* -- Game Over: wait for key press to restart (800ms cooldown) -- */
    if (isDead && millis() - deathTime > 800) {
        if (M5Cardputer.Keyboard.isChange() &&
            M5Cardputer.Keyboard.isPressed()) {
            resetGame();
        }
    }

    delay(16);  // ~60 FPS
}
