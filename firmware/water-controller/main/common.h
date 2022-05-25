enum State {
    START_RUNNING = 0,
    STOP_RUNNING = 1,
    OPENING = 2,
    CLOSING = 3
};

typedef struct {
    uint32_t exp;
    char payload[1500];
} jwt_t;
