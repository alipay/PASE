-- Create the user-defined type for N-dimensional boxes
CREATE FUNCTION pase_in(cstring)
RETURNS pase
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pase_out(pase)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pase_recv(internal)
RETURNS pase
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pase_send(pase)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE pase (
  INTERNALLENGTH = variable,
  INPUT = pase_in,
  OUTPUT = pase_out,
  RECEIVE = pase_recv,
  SEND = pase_send,
  ALIGNMENT = float
);

-- pase constructor[vectors, extra data, distance type(l2 or ip)]
CREATE FUNCTION pase(float4[], int DEFAULT 0, int DEFAULT 0)
RETURNS pase
AS 'MODULE_PATHNAME', 'pase_f4_i_i'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- pase constructor[text vectors, extra data, distance type(l2 or ip)]
CREATE FUNCTION pase(text, int DEFAULT 0, int DEFAULT 0)
RETURNS pase
AS 'MODULE_PATHNAME', 'pase_text_i_i'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- distance function
CREATE FUNCTION g_pase_distance(float4[], pase)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION g_pase_distance_3(text, pase)
RETURNS float4
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- operator on pase
CREATE OPERATOR <?> (
  LEFTARG = float4[], RIGHTARG = pase, PROCEDURE = g_pase_distance,
  COMMUTATOR = '<?>'
);

CREATE OPERATOR <#> (
  LEFTARG = float4[], RIGHTARG = pase, PROCEDURE = g_pase_distance,
  COMMUTATOR = '<#>'
);

CREATE OPERATOR <!> (
  LEFTARG = text, RIGHTARG = pase, PROCEDURE = g_pase_distance_3,
  COMMUTATOR = '<!>'
);

-- hnsw index
CREATE FUNCTION pase_hnsw(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD pase_hnsw TYPE INDEX HANDLER pase_hnsw;

CREATE OPERATOR CLASS pase_hnsw_ops
DEFAULT FOR TYPE float4[] USING pase_hnsw AS
  OPERATOR  1  <?> (float4[], pase) FOR ORDER BY float_ops;

CREATE OPERATOR CLASS pase_hnsw_text_ops
DEFAULT FOR TYPE text USING pase_hnsw AS
  OPERATOR  1  <!> (text, pase) FOR ORDER BY float_ops;

-- ivfflat index
CREATE FUNCTION pase_ivfflat(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD pase_ivfflat TYPE INDEX HANDLER pase_ivfflat;

CREATE OPERATOR CLASS pase_ivfflat_float_ops
DEFAULT FOR TYPE float4[] USING pase_ivfflat AS
  OPERATOR  1  <#> (float4[], pase) FOR ORDER BY float_ops;

CREATE OPERATOR CLASS pase_ivfflat_text_ops
DEFAULT FOR TYPE text USING pase_ivfflat AS
  OPERATOR  1  <!> (text, pase) FOR ORDER BY float_ops;

SET enable_seqscan=off;
SET enable_indexscan=on;

CREATE OR REPLACE FUNCTION ivfflat_search(query_vector text, k integer, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    RETURN QUERY EXECUTE format('
    SELECT id, vector <!> pase(''%s'', 5, 0) AS distance FROM %s ORDER BY vector <!> pase(''%s'', 5, 0) ASC LIMIT %s;
    ', query_vector, table_name, query_vector, k);
END
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION ivfflat_search(query_vector text, k integer, cr integer, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    RETURN QUERY EXECUTE format('
    SELECT id, vector <!> pase(''%s'', %s, 0) AS distance FROM %s ORDER BY vector <!> pase(''%s'', %s, 0) ASC LIMIT %s;
    ', query_vector, cr, table_name, query_vector, cr, k);
END
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION ivfflat_search(query_vector text, k integer, cr integer, query text, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    IF query != '' THEN
        RETURN QUERY EXECUTE format('
            SELECT id, vector <!> pase(''%s'', %s, 0) AS distance FROM %s WHERE %s ORDER BY vector <!> pase(''%s'', %s, 0) ASC LIMIT %s;
            ', query_vector, cr, table_name, query, query_vector, cr, k);
    ELSE
        RETURN QUERY EXECUTE format('
        SELECT id, vector <!> pase(''%s'', %s, 0) AS distance FROM %s ORDER BY vector <!> pase(''%s'', %s, 0) ASC LIMIT %s;
        ', query_vector, cr, table_name, query_vector, cr, k);
    END IF;
END
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION ivfflat_search(query_vector text, k integer, query text, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    IF query != '' THEN
        RETURN QUERY EXECUTE format('
            SELECT id, vector <!> pase(''%s'', 5, 0) AS distance FROM %s WHERE %s ORDER BY vector <!> pase(''%s'', 5, 0) ASC LIMIT %s;
            ', query_vector, table_name, query, query_vector, k);
    ELSE
        RETURN QUERY EXECUTE format('
        SELECT id, vector <!> pase(''%s'', 5, 0) AS distance FROM %s ORDER BY vector <!> pase(''%s'', 5, 0) ASC LIMIT %s;
        ', query_vector, table_name, query_vector, k);
    END IF;
END
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION hnsw_search(query_vector text, ef integer, k integer, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    RETURN QUERY EXECUTE format('
    SELECT id, vector <!> pase(''%s'', %s) AS distance FROM %s ORDER BY vector <!> pase(''%s'', %s) ASC LIMIT %s;
    ', query_vector, ef, table_name, query_vector, ef, k);
END
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION hnsw_search(query_vector text, query text, table_name text) RETURNS TABLE (id text, distance float4) AS $$
BEGIN
    IF query != '' THEN
        RETURN QUERY EXECUTE format('
            SELECT id, vector <!> pase(''%s'') AS distance FROM %s WHERE %s ORDER BY distance ASC LIMIT %s;
            ', query_vector, table_name, query, k);
    ELSE
        RETURN QUERY EXECUTE format('
        SELECT id, vector <!> pase(''%s'') AS distance FROM %s ORDER BY vector <!> pase(''%s'') ASC LIMIT %s;
        ', query_vector, table_name, query_vector, k);
    END IF;
END
$$
LANGUAGE plpgsql;
