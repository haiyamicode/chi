# Chi Analyzer Testing

This directory contains analyzer tests for the Chi compiler to ensure robust error handling of malformed code without crashes.

## Overview

The analyzer testing framework tests the parser's resilience against:
- Incomplete syntax
- Malformed tokens
- Unexpected EOF conditions
- Invalid token sequences
- Nested structure corruption

## Running Analyzer Tests

```bash
make test_analyzer
```

## Test Cases

Each `.xs` file contains intentionally broken Chi code that should produce proper error messages instead of crashing.