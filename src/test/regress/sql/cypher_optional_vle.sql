--
-- Test for OPTIONAL MATCH with variable-length patterns
-- This test ensures that variable-length patterns in OPTIONAL MATCH clauses
-- work correctly and produce the same results as equivalent simple patterns.
--

-- Create test graph
DROP GRAPH IF EXISTS test_optional_vle CASCADE;
CREATE GRAPH test_optional_vle;
SET graph_path = test_optional_vle;

-- Create test data
CREATE VLABEL node;
CREATE ELABEL edge;

-- Create test vertices and edges
CREATE (:node {id: 1})-[:edge]->(:node {id: 2});
CREATE (:node {id: 2})-[:edge]->(:node {id: 3});
CREATE (:node {id: 3})-[:edge]->(:node {id: 1});

-- Test that simple edge pattern and *1..1 pattern with length()=1 produce same results
MATCH (n1:node) 
OPTIONAL MATCH (n1)<-[r]-(n2) 
RETURN count(*);

MATCH (n1:node) 
OPTIONAL MATCH (n1)<-[r*1..1]-(n2) WHERE length(r) = 1 
RETURN count(*);

-- Test *0..1 pattern behavior
MATCH (n1:node) 
OPTIONAL MATCH (n1)<-[r*0..1]-(n2) 
RETURN count(*);

-- Test that zero-length patterns (*0..0) work correctly
MATCH (n1:node) 
OPTIONAL MATCH (n1)<-[r*0..0]-(n2) 
RETURN count(*);

-- Clean up
DROP GRAPH test_optional_vle CASCADE;