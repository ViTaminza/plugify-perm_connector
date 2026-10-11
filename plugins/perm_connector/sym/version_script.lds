{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* permissions binding slots (defined by permissions_bindings, filled by the plugify
           module from the permissions plugin's exported methods) */
        __permissions_*;

        /* No exported methods: perm_connector exposes nothing to other plugins. */

    local: *;
};
