#ifndef __MODSEARCH_H
#define __MODSEARCH_H

#include "../mozartcore.hh"
#include <gecode/search.hh>

namespace mozart {
namespace builtins {

///////////////////
// Search Module //
///////////////////

class ModSearch: public Module {
public:
	ModSearch(): Module("Search") {}

	class DFS: public Builtin<DFS> {
	public:
		DFS(): Builtin("dfs") {}

	    static void call(VM vm, In space) {
			if(ConstraintSpace(space).isConstraintSpace(vm)) { 
			  //result = SpaceLike(space).cloneSpace(vm);
			  GecodeSpace* gs = ConstraintSpace(space).constraintSpace(vm);
			  GecodeSpace* aux = (GecodeSpace*)gs->clone();
			  Gecode::DFS<GecodeSpace> e(aux);
			  std::cout << "prop gs in builtin: " << gs->propagators() << std::endl;
			  *gs = *e.next();
			  std::cout << "memoria gs in builtin: " << gs << std::endl;
			  std::cout << "memoria aux in builtin: " << aux << std::endl;
			  std::cout << "status gs in builtin: " << gs->status() << std::endl;
			  std::cout << "status aux in builtin: " << aux->status() << std::endl;

					
			}

		}

	};

};

} // namespace builtins
} // namespace mozart
#endif // __MODSEARCH_H


