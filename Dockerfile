# Build stage: compile the bot as one static binary.
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY main.cpp json.hpp ./
RUN g++ -O2 -std=c++17 -static -o /mechbot main.cpp

# Runtime stage: just the binary. The map is mounted read-only at /arena by the match.
FROM ubuntu:24.04
COPY --from=build /mechbot /mechbot
USER 65534:65534
ENTRYPOINT ["/mechbot"]
