-- Load the module in every new backend of the cluster: the qExecs send
-- their data only where it is loaded, and LOAD reaches only the backends
-- running now.
--start_ignore
\! gpconfig -c session_preload_libraries -v "$(psql -At -c "SELECT array_to_string(array_append(string_to_array(current_setting('session_preload_libraries'), ','), 'test_explain_hooks'), ',')" postgres)"
\! gpstop -u
--end_ignore
