#pragma once

// Lightweight settings object for the AI backends. Replaces the original
// project's much larger Config; only the knobs the clients actually read are
// kept here, with sane defaults. Wire these to the mini-code config store when
// the clients are integrated into the CLI.
class AiConfig {
public:
    int maxOutputTokens() const { return m_maxOutputTokens; }
    int maxToolIterations() const { return m_maxToolIterations; }
    int openaiConnectionTimeoutSeconds() const { return m_connectionTimeoutSeconds; }
    int openaiReadTimeoutSeconds() const { return m_readTimeoutSeconds; }

    void setMaxOutputTokens(int v) { m_maxOutputTokens = v; }
    void setMaxToolIterations(int v) { m_maxToolIterations = v; }
    void setOpenaiConnectionTimeoutSeconds(int v) { m_connectionTimeoutSeconds = v; }
    void setOpenaiReadTimeoutSeconds(int v) { m_readTimeoutSeconds = v; }

private:
    int m_maxOutputTokens = 16000;
    int m_maxToolIterations = 50;
    int m_connectionTimeoutSeconds = 30;
    int m_readTimeoutSeconds = 300;
};
