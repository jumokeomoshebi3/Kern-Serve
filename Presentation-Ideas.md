# Presentation Ideas

* Obviously, go over the differences between a kernel module C program and a regular C program (`__init` vs. `main()`, etc.)

* Compare Kernel Thread to regular thread (like the ones we've used in class)


#### Here's a fun quote:
"There is a tendency to think of LKMs like user space programs. They do share a lot of their properties, but LKMs are definitely not user space programs. They are part of the kernel. As such, they have free run of the system and can easily crash it." (<a href="https://tldp.org/HOWTO/Module-HOWTO/x73.html" target="_blank">Linux Loadable Kernel Module HOWTO</a>)


Point out there's no thread_join. It just spawns it up and it's off to the races.


## Questions to Answer


## TO DO
Don't forget to add attributions in code
