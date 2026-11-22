#if defined(NWNX_SQL_POSTGRESQL_SUPPORT)

#include "nwnx.hpp"
#include <iostream>
#include <cstdlib>
#include <cctype>

#include "PostgreSQL.hpp"
using namespace NWNXLib;

static bool s_nextQueryBinaryResults;

NWNX_EXPORT ArgumentStack PostgreSQL_SetNextQueryResultsBinaryMode(ArgumentStack&&)
{
    LOG_DEBUG("Next query only: Results will be returned in binary mode.");
    s_nextQueryBinaryResults = true;
    return {};
}

namespace SQL {

PostgreSQL::PostgreSQL()
{
    // Not sure if we need to do anything here.
}

PostgreSQL::~PostgreSQL()
{
    PQfinish(m_conn);
}

void PostgreSQL::Connect()
{
    const auto host = Config::Get<std::string>("HOST", "localhost");
    const auto port = Config::Get<std::string>("PORT", "5432");
    const auto user = Config::Get<std::string>("USERNAME");
    const auto pass = Config::Get<std::string>("PASSWORD");
    const auto sslmode = Config::Get<std::string>("SSL_MODE", "disable");
    // Database is optional, libpq defaults to the database named after USERNAME.
    const auto db = Config::Get<std::string>("DATABASE");
    const auto charset = Config::Get<std::string>("CHARACTER_SET");

    std::vector<const char*> keys = { "host", "port", "sslmode", "connect_timeout",
        "keepalives", "keepalives_idle", "keepalives_interval", "keepalives_count" };
    std::vector<const char*> values = { host.c_str(), port.c_str(), sslmode.c_str(), "5", "1", "60", "10", "3" };
    if (user) { keys.push_back("user"); values.push_back(user->c_str()); }
    if (pass) { keys.push_back("password"); values.push_back(pass->c_str()); }
    if (db) { keys.push_back("dbname"); values.push_back(db->c_str()); }
    // Set at connect time so PQreset() keeps it.
    if (charset) { keys.push_back("client_encoding"); values.push_back(charset->c_str()); }
    keys.push_back(nullptr);
    values.push_back(nullptr);

    LOG_INFO("Connection info:  host=%s port=%s dbname=%s user=%s sslmode=%s",
        host, port, db ? *db : "", user ? *user : "", sslmode);
    if (charset)
        LOG_INFO("Connection character set is '%s'", *charset);

    PQfinish(m_conn);
    m_conn = PQconnectdbParams(keys.data(), values.data(), 0);

    if (PQstatus(m_conn) != CONNECTION_OK)
    {
        throw std::runtime_error(std::string("Error connecting to Postgres DB: ") + PQerrorMessage(m_conn));
    }

    s_nextQueryBinaryResults = false;
}

bool PostgreSQL::IsConnected()
{
    // libpq only notices a lost session on the next I/O; PrepareQuery() resets it then.
    return PQstatus(m_conn) == CONNECTION_OK;
}

bool PostgreSQL::PrepareQuery(const Query& query)
{
    LOG_DEBUG("Preparing query %s\n", query);

    m_affectedRows = -1;

    // libpq needs the highest $N used, there is no call to ask the server for it.
    m_paramCount = 0;
    for (size_t i = 0; i < query.size(); i++)
    {
        if (query[i] != '$' || (i > 0 && (std::isalnum((unsigned char)query[i-1]) ||
            query[i-1] == '_' || query[i-1] == '$' || query[i-1] == '\\')))
            continue;
        size_t n = 0;
        while (i + 1 < query.size() && std::isdigit((unsigned char)query[i+1]))
            n = n * 10 + (query[++i] - '0');
        m_paramCount = std::max(m_paramCount, n);
    }

    LOG_DEBUG("Detected %d parameters.", m_paramCount);

    m_params.resize(m_paramCount);
    m_formats.resize(m_paramCount);
    m_lengths.resize(m_paramCount);

    PGresult *res = PQprepare(m_conn,      // connection
                        "",                // statement name, blank in this case.
                        query.c_str(),     // query string
                        m_paramCount,      // param count
                        NULL);             // param types (can be null to infer)

    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQstatus(m_conn) == CONNECTION_BAD)
    {
        LOG_WARNING("Database connection lost. Reconnecting..");
        PQclear(res);
        PQreset(m_conn);
        res = PQprepare(m_conn, "", query.c_str(), m_paramCount, NULL);
    }

    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        m_lastError.assign(res ? PQresultErrorMessage(res) : PQerrorMessage(m_conn));
        LOG_WARNING("Query '%s' failed due to error '%s'", query, m_lastError);
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

std::optional<ResultSet> PostgreSQL::ExecuteQuery()
{

    m_affectedRows = -1;

    std::vector<char*> paramValues(m_paramCount);
    for (size_t i = 0; i < m_paramCount; i++)
    {
        if (!m_params[i])
            continue;
        if (m_formats[i] == 1)
        {
            size_t toLen;
            paramValues[i] = (char*) PQescapeByteaConn(m_conn,
                (const unsigned char*) m_params[i]->data(), m_params[i]->size(), &toLen);
        }
        else
        {
            paramValues[i] = m_params[i]->data();
        }
    }

    PGresult *res = PQexecPrepared(
        m_conn,                                 // connection
        "",                                     // statement name (same as in the prepare above)
        m_paramCount,                           // m_paramCount from previous
        paramValues.data(),                     // param data (can be null)
        // NB: Both of these are null; all data passed in is text mode.
        //     bytea data gets escaped automatically by setting m_format[i] in PrepareBinary().
        NULL,                                   // param lengths - only for binary data
        NULL,                                   // param formats - server will infer text
        0);                                     // result format, 0=text, 1=binary

    for (size_t i = 0; i < m_paramCount; i++)
    {
        if (m_formats[i] == 1)
            PQfreemem(paramValues[i]);
    }

    // Rows returned - collect and pass on
    if (PQresultStatus(res) == PGRES_TUPLES_OK)
    {
        ResultSet results;
        const size_t rows = PQntuples(res);
        const size_t cols = PQnfields(res);
        LOG_DEBUG("Returning %d rows of %d columns.", rows, cols);

        for(int i=0; i<(int)rows; i++)
        {
            ResultRow row;
            row.reserve(cols);
            for (int j=0; j<(int)cols; j++)
            {
                const char* ptr = PQgetvalue(res, i, j);
                size_t ptrLen = PQgetlength(res, i, j);
                if (j == 0 && s_nextQueryBinaryResults)
                {
                    LOG_DEBUG("Forcefully unescaping query result column %d.", j);
                    if (auto* unescaped = PQunescapeBytea((const unsigned char*) ptr, &ptrLen))
                    {
                        row.emplace_back((char*) unescaped, ptrLen);
                        PQfreemem(unescaped);
                        continue;
                    }
                    ptrLen = 0;
                }

                row.emplace_back(ptr, ptrLen);
            }
            results.push(row);
        }

        // Always disable binary mode, even if no columns with it were read.
        s_nextQueryBinaryResults = false;

        PQclear(res);
        return std::make_optional<ResultSet>(std::move(results)); // Succeeded query, succeeded results.
    }

    // DML that doesn't return rows (inserts, updates, etc.)
    // Capture the rows affected if applicable.
    if (PQresultStatus(res) == PGRES_COMMAND_OK)
    {
        LOG_DEBUG("Fetching rows affected by command.");
        const char *cnt = PQcmdTuples(res);
        if (*cnt != '\0')
        {
            m_affectedRows = atoi(cnt);
        }
        PQclear(res);
        return std::make_optional<ResultSet>(ResultSet()); // Succeeded query, no results.
    }

    // Else.. something unexpected happened.

    const char* error = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
    if (!error)
        error = PQerrorMessage(m_conn);
    if (*error == '\0')
    {
        // No valid error.
        error = "Undefined/unknown";
    }

    // Save a copy of the error.  In PgSQL, the error comes from the result we got from the server,
    // which we're about to clear.
    m_lastError.assign(error);

    PQclear(res);
    return std::optional<ResultSet>();
}

// Parameters are just passed as strings.  PgSQL figures out what it's supposed to be and casts if necessary.
void PostgreSQL::PrepareInt(int32_t position, int32_t value)
{
    LOG_DEBUG("Assigning position %d to value '%d'", position, value);
    m_params[position] = std::to_string(value);
    m_formats[position] = 0;
}
void PostgreSQL::PrepareFloat(int32_t position, float value)
{
    LOG_DEBUG("Assigning position %d to value '%f'", position, value);
    m_params[position] = std::to_string(value);
    m_formats[position] = 0;
}
void PostgreSQL::PrepareString(int32_t position, const std::string& value)
{
    LOG_DEBUG("Assigning position %d to value '%s'", position, value);
    m_params[position] = value;
    m_formats[position] = 0;
}

void PostgreSQL::PrepareBinary(int32_t position, const std::vector<uint8_t> &value)
{
    LOG_DEBUG("Assigning position %d to %d bytes of binary data", position, value.size());
    m_params[position] = std::string(value.begin(), value.end());
    m_lengths[position] = value.size();
    m_formats[position] = 1;
}
void PostgreSQL::PrepareNULL(int32_t position)
{
    LOG_DEBUG("Assigning position %d to value NULL", position);
    m_params[position] = std::nullopt;
    m_formats[position] = 0;
}

int PostgreSQL::GetAffectedRows()
{
    return m_affectedRows;
}

std::string PostgreSQL::GetLastError(bool bClear)
{
    // This might be overkill, but copy the string here so the class stored string can be cleared
    // before returning.
    std::string temp = m_lastError;
    if (bClear)
        m_lastError.clear();
    return temp;
}

int32_t PostgreSQL::GetPreparedQueryParamCount()
{
    return m_paramCount;
}

void PostgreSQL::DestroyPreparedQuery()
{
    // No way or need to deallocate the anonymous prepared statement in PgSQL.

    // Force deallocation
    std::vector<std::optional<std::string>>().swap(m_params);
    m_paramCount = 0;

    s_nextQueryBinaryResults = false;
}

}
#endif
