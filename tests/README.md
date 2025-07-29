# Test Directory Structure Guide

This project adopts a three-tier testing strategy (Test Pyramid):

## 1. Unit Tests (tests/unit/) - 70%
**Goal**: Test individual component functionality, fast feedback, high coverage
**Characteristics**: 
- Use mocks to isolate dependencies
- Fast execution (millisecond level)
- Test single functionality points

**Directory Structure**:
```
tests/unit/
├── protocol/
│   ├── test_binary_protocol.cpp     # Binary protocol encoding/decoding tests
│   └── test_http_handler.cpp        # HTTP handler tests
├── core/
├── gateway/
└── script/
```

## 2. Integration Tests (tests/integration/) - 20%
**Goal**: Test interactions between components, verify interface contracts
**Characteristics**:
- Test multiple component collaboration
- Use real components, minimize mocks
- Moderate execution time (second level)

**Directory Structure**:
```
tests/integration/
├── components/
│   └── test_gateway_component.cpp   # Gateway component integration tests
├── protocol/
└── network/
```

## 3. End-to-End Tests (tests/e2e/) - 10%
**Goal**: Test complete user scenarios, verify overall system functionality
**Characteristics**:
- Requires real running server
- Test complete user workflows
- Longer execution time (minute level)
- Usually requires manual environment setup

**Directory Structure**:
```
tests/e2e/
└── test_full_system.cpp             # Complete system end-to-end tests
```

## Execution Guide

### Unit Tests 
```bash
# Run all unit tests (fast, run every time in CI)
ctest -R "unit_"
```

### Integration Tests
```bash
# Run integration tests (moderate speed, run in CI)
ctest -R "integration_"
```

### End-to-End Tests
```bash
# First start the server
./shield --config=config/test.yaml

# Then run end-to-end tests (slow, usually run before release)
ctest -R "e2e_"
```

## Testing Best Practices

1. **Test Naming**: Follow `Test[Function][Scenario]` format
2. **Test Isolation**: Each test is independent, can run in parallel
3. **Data Management**: Use test-specific temporary data
4. **Error Scenarios**: Test not only normal flows but also error handling
5. **Performance Verification**: Include performance assertions for critical paths

## Issues with Original Integration Tests

The original `test_integration.cpp` was actually end-to-end tests, with the following issues:
- Dependent on external server running
- Difficult to locate problems when tests fail
- Unstable execution, affected by network and environment

Through refactoring to three-tier architecture, we can achieve:
- Improved test reliability and speed
- Better error localization
- Support for CI/CD automation
- Better test coverage reporting