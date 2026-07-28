import re
import os

def fix_test_file(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Pattern to find test cases that use LuaServiceManager but don't have actor_system
    # We need to add actor_system initialization before the manager creation
    
    # Find all BOOST_AUTO_TEST_CASE blocks
    test_case_pattern = r'(BOOST_AUTO_TEST_CASE\([^)]+\)\s*\{)'
    
    def add_actor_system(match):
        test_case_header = match.group(1)
        # Check if this test case already has actor_system
        # We'll handle this by looking at the full test case content
        return test_case_header
    
    # Split content into test cases
    parts = re.split(r'(BOOST_AUTO_TEST_CASE\([^)]+\)\s*\{)', content)
    
    result = []
    i = 0
    while i < len(parts):
        if i + 1 < len(parts) and parts[i].startswith('BOOST_AUTO_TEST_CASE'):
            # This is a test case header
            test_header = parts[i]
            # Get the test case body (until the next test case or end)
            body_start = i + 1
            # Find the end of this test case (matching braces)
            brace_count = 0
            body_end = body_start
            for j in range(body_start, len(parts)):
                brace_count += parts[j].count('{') - parts[j].count('}')
                if brace_count <= 0:
                    body_end = j
                    break
            
            # Get the full test case content
            test_body = ''.join(parts[body_start:body_end+1])
            
            # Check if this test case uses LuaServiceManager
            if 'LuaServiceManager' in test_body and 'actor_system' not in test_body:
                # Need to add actor_system initialization
                # Find the first LuaRuntime line
                runtime_pattern = r'(\s+)(LuaRuntime\s+\w+;)'
                match = re.search(runtime_pattern, test_body)
                if match:
                    indent = match.group(1)
                    runtime_line = match.group(2)
                    # Add actor_system before LuaRuntime
                    new_body = test_body.replace(
                        runtime_line,
                        f'{indent}caf::actor_system_config cfg;\n{indent}caf::actor_system system(cfg);\n{indent}{runtime_line}'
                    )
                    result.append(test_header)
                    result.append(new_body)
                else:
                    result.append(test_header)
                    result.append(test_body)
            else:
                result.append(test_header)
                result.append(test_body)
            
            i = body_end + 1
        else:
            result.append(parts[i])
            i += 1
    
    new_content = ''.join(result)
    
    with open(filepath, 'w') as f:
        f.write(new_content)

# Fix all test files
for filename in os.listdir('tests/lua_api'):
    if filename.endswith('.cpp'):
        filepath = os.path.join('tests/lua_api', filename)
        print(f'Fixing {filepath}...')
        fix_test_file(filepath)

print('Done!')
